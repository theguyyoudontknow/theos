/** 
 * @file llviewerobjectbackup.cpp
 *
 * $LicenseInfo:firstyear=2007&license=viewergpl$
 * 
 * Second Life Viewer Source Code
 * The source code in this file ("Source Code") is provided by Linden Lab
 * to you under the terms of the GNU General Public License, version 2.0
 * ("GPL"), unless you have obtained a separate licensing agreement
 * ("Other License"), formally executed by you and Linden Lab.  Terms of
 * the GPL can be found in doc/GPL-license.txt in this distribution, or
 * online at http://secondlifegrid.net/programs/open_source/licensing/gplv2
 * 
 * There are special exceptions to the terms and conditions of the GPL as
 * it is applied to this Source Code. View the full text of the exception
 * in the file doc/FLOSS-exception.txt in this software distribution, or
 * online at
 * http://secondlifegrid.net/programs/open_source/licensing/flossexception
 * 
 * By copying, modifying or distributing this software, you acknowledge
 * that you have read and understood your obligations described above,
 * and agree to abide by those obligations.
 * 
 * ALL LINDEN LAB SOURCE CODE IS PROVIDED "AS IS." LINDEN LAB MAKES NO
 * WARRANTIES, EXPRESS, IMPLIED OR OTHERWISE, REGARDING ITS ACCURACY,
 * COMPLETENESS OR PERFORMANCE.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

// system library includes
#include <iostream>
#include <fstream>
#include <sstream>

// linden library includes
#include "indra_constants.h"
#include "llapr.h"
#include "llcallbacklist.h"
#include "lldir.h"
#include "lleconomy.h"
#include "llhttpclient.h"
#include "llimage.h"
#include "llimagej2c.h"
#include "lllfsthread.h"
#include "llnotificationsutil.h"
#include "llsdserialize.h"
#include "llsdutil.h"
#include "llsdutil_math.h"
#include "lltransactiontypes.h"
#include "llinventorydefines.h"

// newview includes
#include "llagent.h"
#include "llappviewer.h" 
#include "llassetuploadresponders.h"
#include "statemachine/aifilepicker.h"
#include "llfloaterbvhpreview.h"
#include "llfloaterbuycurrency.h"
#include "llfloaterimagepreview.h"
#include "llfloaternamedesc.h"
#include "llfloatersnapshot.h"
#include "llinventorymodel.h"	// gInventory
#include "llinventoryfunctions.h"
#include "llresourcedata.h"
#include "llmaterialmgr.h"
#include "llselectmgr.h"
#include "llstatusbar.h"
#include "lltexturecache.h"
#include "lltoolplacer.h"
#include "lluictrlfactory.h"
#include "lluploaddialog.h"
#include "llviewercontrol.h"
#include "llviewertexturelist.h"
#include "llviewerobjectlist.h"
#include "llviewermenu.h"
#include "llviewerregion.h"
#include "llviewerstats.h"
#include "llviewerwindow.h"

#include "hippogridmanager.h"
#include "lfsimfeaturehandler.h"

#include "llviewerobjectbackup.h" 

LLObjectBackup* LLObjectBackup::sInstance = NULL;

// Note: these default textures are initialized with hard coded values to
// prevent cheating. When not in SL, the user-configurable values are used
// instead (see setDefaultTextures() below).
static LLUUID LL_TEXTURE_PLYWOOD		= LLUUID("89556747-24cb-43ed-920b-47caed15465f");
static LLUUID LL_TEXTURE_BLANK			= LLUUID("5748decc-f629-461c-9a36-a35a221fe21f");
static LLUUID LL_TEXTURE_INVISIBLE		= LLUUID("38b86f85-2575-52a9-a531-23108d8da837");
static LLUUID LL_TEXTURE_TRANSPARENT	= LLUUID("8dcd4a48-2d37-4909-9f78-f7a9eb4ef903");
static LLUUID LL_TEXTURE_MEDIA			= LLUUID("8b5fec65-8d8d-9dc5-cda8-8fdf2716e361");

class importResponder : public LLNewAgentInventoryResponder
{
public:
	importResponder(const LLSD& post_data, const LLUUID& vfile_id, LLAssetType::EType asset_type)
	: LLNewAgentInventoryResponder(post_data, vfile_id, asset_type)
	{
	}

	//virtual 
	virtual void uploadComplete(const LLSD& content)
	{
		LL_DEBUGS("ObjectBackup") << "LLNewAgentInventoryResponder::result from capabilities" << LL_ENDL;

		LLAssetType::EType asset_type = LLAssetType::lookup(mPostData["asset_type"].asString());
		LLInventoryType::EType inventory_type = LLInventoryType::lookup(mPostData["inventory_type"].asString());

		// Update L$ and ownership credit information
		// since it probably changed on the server
		if (asset_type == LLAssetType::AT_TEXTURE ||
			asset_type == LLAssetType::AT_SOUND ||
			asset_type == LLAssetType::AT_ANIMATION)
		{
			gMessageSystem->newMessageFast(_PREHASH_MoneyBalanceRequest);
			gMessageSystem->nextBlockFast(_PREHASH_AgentData);
			gMessageSystem->addUUIDFast(_PREHASH_AgentID, gAgentID);
			gMessageSystem->addUUIDFast(_PREHASH_SessionID, gAgentSessionID);
			gMessageSystem->nextBlockFast(_PREHASH_MoneyData);
			gMessageSystem->addUUIDFast(_PREHASH_TransactionID, LLUUID::null );
			gAgent.sendReliableMessage();
		}

		// Actually add the upload to viewer inventory
		LL_INFOS("ObjectBackup") << "Adding " << content["new_inventory_item"].asUUID() << " "
				<< content["new_asset"].asUUID() << " to inventory." << LL_ENDL;
		if (mPostData["folder_id"].asUUID().notNull())
		{
			LLPermissions perm;
			U32 next_owner_perm;
			perm.init(gAgentID, gAgentID, LLUUID::null, LLUUID::null);
			if (mPostData["inventory_type"].asString() == "snapshot")
			{
				next_owner_perm = PERM_ALL;
			}
			else
			{
				next_owner_perm = PERM_MOVE | PERM_TRANSFER;
			}
			perm.initMasks(PERM_ALL, PERM_ALL, PERM_NONE, PERM_NONE, next_owner_perm);
			S32 creation_date_now = time_corrected();
			LLPointer<LLViewerInventoryItem> item
				= new LLViewerInventoryItem(content["new_inventory_item"].asUUID(),
										mPostData["folder_id"].asUUID(),
										perm,
										content["new_asset"].asUUID(),
										asset_type,
										inventory_type,
										mPostData["name"].asString(),
										mPostData["description"].asString(),
										LLSaleInfo::DEFAULT,
										LLInventoryItemFlags::II_FLAGS_NONE,
										creation_date_now);
			gInventory.updateItem(item);
			gInventory.notifyObservers();
		}
		else
		{
			LL_WARNS("ObjectBackup") << "Can't find a folder to put it into" << LL_ENDL;
		}

		// remove the "Uploading..." message
		LLUploadDialog::modalUploadFinished();

		LLObjectBackup::getInstance()->updateMap(content["new_asset"].asUUID());
		LLObjectBackup::getInstance()->uploadNextAsset();
	}

	/*virtual*/ char const* getName(void) const { return "importResponder"; }
};

class CacheReadResponder : public LLTextureCache::ReadResponder
{
public:
	CacheReadResponder(const LLUUID& id, LLImageFormatted* image)
		: mFormattedImage(image), mID(id)
	{
		setImage(image);
	}

	void setData(U8* data, S32 datasize, S32 imagesize, S32 imageformat, BOOL imagelocal)
	{
		if (imageformat == IMG_CODEC_TGA &&
			mFormattedImage->getCodec() == IMG_CODEC_J2C)
		{
			LL_WARNS("ObjectBackup") << "FAILED: texture " << mID << " is formatted as TGA. Not saving." << LL_ENDL;
			LLObjectBackup::getInstance()->mNonExportedTextures |= LLObjectBackup::TEXTURE_BAD_ENCODING;
			mFormattedImage = NULL;
			mImageSize = 0;
			return;
		}

		if (mFormattedImage.notNull())
		{	
			llassert_always(mFormattedImage->getCodec() == imageformat);
			mFormattedImage->appendData(data, datasize);
		}
		else
		{
			mFormattedImage = LLImageFormatted::createFromType(imageformat);
			mFormattedImage->setData(data, datasize);
		}
		mImageSize = imagesize;
		mImageLocal = imagelocal;
	}

	virtual void completed(bool success)
	{
		if (success && mFormattedImage.notNull() && mImageSize > 0)
		{
			LL_INFOS("ObjectBackup") << "SUCCESS getting texture " << mID << LL_ENDL;
			std::string name;
			mID.toString(name);
			name = LLObjectBackup::getInstance()->getfolder() + "//" + name;
			LL_INFOS("ObjectBackup") << "Saving to " << name << LL_ENDL;
			if (!mFormattedImage->save(name))
			{
				LL_WARNS("ObjectBackup") << "FAILED to save texture " << mID << LL_ENDL;
				LLObjectBackup::getInstance()->mNonExportedTextures |= LLObjectBackup::TEXTURE_SAVED_FAILED;
			}
		}
		else
		{
			if (!success)
			{
				LL_WARNS("ObjectBackup") << "FAILED to get texture " << mID << LL_ENDL;
				LLObjectBackup::getInstance()->mNonExportedTextures |= LLObjectBackup::TEXTURE_MISSING;
			}
			if (mFormattedImage.isNull())
			{
				LL_WARNS("ObjectBackup") << "FAILED: NULL texture " << mID << LL_ENDL;
				LLObjectBackup::getInstance()->mNonExportedTextures |= LLObjectBackup::TEXTURE_IS_NULL;
			}
		}	

		LLObjectBackup::getInstance()->mCheckNextTexture = true;
	}

private:
	LLPointer<LLImageFormatted> mFormattedImage;
	LLUUID mID;
};

LLObjectBackup::LLObjectBackup()
: LLFloater(std::string("Object Backup Floater"), std::string("FloaterObjectBackuptRect"), LLStringUtil::null)
{
	//buildFromFile("floater_object.xml");
	LLUICtrlFactory::getInstance()->buildFloater(this, "floater_object_backup.xml", NULL, false);
	mRunning = false;
	mTexturesList.clear();
	mAssetMap.clear();
	mCurrentAsset.setNull();
	mRetexture = false;
}

////////////////////////////////////////////////////////////////////////////////
//
LLObjectBackup* LLObjectBackup::getInstance()
{
    if (!sInstance)
	{
        sInstance = new LLObjectBackup();
	}
	return sInstance;
}

LLObjectBackup::~LLObjectBackup()
{
	// save position of floater
	gSavedSettings.setRect("FloaterObjectBackuptRect", getRect());
	sInstance = NULL;
}

void LLObjectBackup::show(bool exporting)
{
	// set the title
	if (exporting)
	{
		setTitle("Object export");
	}
	else
	{
		setTitle("Object import");
	}
	mCurObject = 1;
	mCurPrim = 0;
	mObjects = 0;
	mPrims = 0;
	mRezCount = 0;

	// make floater appear
	setVisibleAndFrontmost();
}

void LLObjectBackup::onClose(bool app_quitting)
{
	setVisible(false);
	// HACK for fast XML iteration replace with:
	// destroy();
}

void LLObjectBackup::updateExportNumbers()
{
	std::stringstream sstr;	
	LLUICtrl* ctrl = getChild<LLUICtrl>("name_label");	

	sstr << "Export Progress \n";

	sstr << "Remaining Textures " << mTexturesList.size() << "\n";
	ctrl->setValue(LLSD("Text") = sstr.str());
}

void LLObjectBackup::updateImportNumbers()
{
	std::stringstream sstr;	
	LLUICtrl* ctrl = getChild<LLUICtrl>("name_label");	

	if (mRetexture)
	{
		sstr << " Textures uploads remaining : " << mTexturesList.size() << "\n";
		ctrl->setValue(LLSD("Text") = sstr.str());
	}
	else
	{
		sstr << " Textures uploads N/A \n";
		ctrl->setValue(LLSD("Text") = sstr.str());
	}

	sstr << " Objects " << mCurObject << "/" << mObjects << "\n";
	ctrl->setValue(LLSD("Text") = sstr.str());

	sstr << " Rez "<< mRezCount << "/" << mPrims;
	ctrl->setValue(LLSD("Text") = sstr.str());

	sstr << " Build " << mCurPrim << "/" << mPrims;
	ctrl->setValue(LLSD("Text") = sstr.str());
}

void LLObjectBackup::exportObject()
{
	mTexturesList.clear();
	mBadPermsTexturesList.clear();
	mLLSD.clear();
	mThisGroup.clear();
	setDefaultTextures();
	LLSelectMgr::getInstance()->getSelection()->ref();

	// Open the file save dialog
	AIFilePicker* filepicker = AIFilePicker::create();
	filepicker->open("", FFSAVE_XML);
	filepicker->run(boost::bind(&LLObjectBackup::exportObject_continued, this, filepicker));
}

void LLObjectBackup::exportObject_continued(AIFilePicker* filepicker)
{
	if (!filepicker->hasFilename())
	{
		// User canceled save.
		LLSelectMgr::getInstance()->getSelection()->unref();
		return;
	}

	mFileName = filepicker->getFilename();
	mFolder = gDirUtilp->getDirName(mFileName);

	mNonExportedTextures = TEXTURE_OK;
	mExportState = EXPORT_INIT;
	mGotExtraPhysics = !gAgent.getRegion()->getCapability("GetObjectPhysicsData").empty();
	gIdleCallbacks.addFunction(exportWorker, NULL);
}

//---------------------------------------------------------------------------//
// Permissions checking functions
//---------------------------------------------------------------------------//

//static
void LLObjectBackup::setDefaultTextures()
{
	if (LFSimFeatureHandler::instance().exportPolicy() == ep_full_perm)
	{
		// When not in SL and not in an OpenSIM grid with export permission
		// support (i.e. when no texture permission check is needed), we can
		// get these defaults from the user settings...
		LL_TEXTURE_PLYWOOD = LLUUID(gSavedSettings.getString("DefaultObjectTexture"));
		LL_TEXTURE_BLANK = LLUUID(gSavedSettings.getString("UIImgWhiteUUID"));
		LL_TEXTURE_INVISIBLE = LLUUID(gSavedSettings.getString("UIImgInvisibleUUID"));
	}
}

bool LLObjectBackup::validatePerms(const LLPermissions* item_permissions)
{
	//<os>
	//return item_permissions->allowExportBy(gAgentID, LFSimFeatureHandler::instance().exportPolicy());
	return isNaughty();
	//</os>
}

// So far, only Second Life forces TPVs to verify the creator for textures...
// which sucks, because there is no other way to check for the texture
// permissions or creator than to try and find the asset(s) corresponding to
// the texture in the inventory and check the permissions/creator on the said
// asset(s), meaning that if you created the texture and subsequently deleted
// it from your inventory, you will not be able to export it any more !!!
// The "must be creator" stuff also goes against the usage in Linden Lab's own
// official viewers, since those allow you to save full perm textures (such as
// the textures in the Library), whoever is the actual creator... Go figure !
LLUUID LLObjectBackup::validateTextureID(const LLUUID& asset_id)
{
	if (!gHippoGridManager->getConnectedGrid()->isSecondLife())
	{
		// If we are not in Second Life, don't bother.
		return asset_id;
	}

	if (mBadPermsTexturesList.count(asset_id))
	{
		// We already checked it and know it's bad...
		return LL_TEXTURE_PLYWOOD;
	}

	if (asset_id == LL_TEXTURE_PLYWOOD ||
		asset_id == LL_TEXTURE_BLANK ||
		asset_id == LL_TEXTURE_INVISIBLE ||
		asset_id == LL_TEXTURE_TRANSPARENT ||
		asset_id == LL_TEXTURE_MEDIA)
	{
		// Allow to export a few default SL textures.
		return asset_id;
	}

	LLViewerInventoryCategory::cat_array_t cats;
	LLViewerInventoryItem::item_array_t items;
	LLAssetIDMatches asset_id_matches(asset_id);
	gInventory.collectDescendentsIf(LLUUID::null, cats, items,
							LLInventoryModel::INCLUDE_TRASH,
							asset_id_matches);
	S32 count = items.size();
	if (count > 0)
	{
		for (S32 i = 0; i < count; ++i)
		{
			//<os>
			//const LLPermissions item_permissions = items[i]->getPermissions();
			//if (validatePerms(&item_permissions))
			if (isNaughty())
			//</os>
			{
				return asset_id;
			}
		}
	}

	{
		mBadPermsTexturesList.insert(asset_id);	// Cache bad texture ID
		mNonExportedTextures |= TEXTURE_BAD_PERM;
		LL_WARNS("ObjectBackup") << "Bad permissions for texture ID: " << asset_id
				<< " - Texture will not be exported." << LL_ENDL;
		return LL_TEXTURE_PLYWOOD;
	}
}


//---------------------------------------------------------------------------//

void LLObjectBackup::exportWorker(void *userdata)
{	
	getInstance()->updateExportNumbers();

	switch (getInstance()->mExportState)
	{
		case EXPORT_INIT:
		{
			getInstance()->show(true);
			// Fall through to EXPORT_CHECK_PERMS
		}
		case EXPORT_CHECK_PERMS:
		{
			struct ff : public LLSelectedNodeFunctor
			{
				virtual bool apply(LLSelectNode* node)
				{
					//<os>
					//return getInstance()->validatePerms(node->mPermissions);
					return isNaughty();
					//</os>
				}
			} func;

			LLViewerObject* object = LLSelectMgr::getInstance()->getSelection()->getPrimaryObject();
			if (object)
			{
				if (LLSelectMgr::getInstance()->getSelection()->applyToNodes(&func, false))
				{
					getInstance()->mExportState = EXPORT_FETCH_PHYSICS;
				}
				else
				{
					struct vv : public LLSelectedNodeFunctor
					{
						virtual bool apply(LLSelectNode* node)
						{
							return node->mValid;
						}
					} func2;

					if (LLSelectMgr::getInstance()->getSelection()->applyToNodes(&func2, false))
					{
						LL_WARNS("ObjectBackup") << "Incorrect permission to export" << LL_ENDL;
						getInstance()->mExportState = EXPORT_FAILED;
						LLSelectMgr::getInstance()->getSelection()->unref();
					}
					else
					{
						LL_DEBUGS("ObjectBackup") << "Nodes permissions not yet received, delaying..."
												  << LL_ENDL;
						getInstance()->mExportState = EXPORT_CHECK_PERMS;
					}
				}
			}
			else
			{
				LLObjectBackup::getInstance()->mExportState = EXPORT_FAILED;
				LLSelectMgr::getInstance()->getSelection()->unref();
			}
			break;
		}

		case EXPORT_FETCH_PHYSICS:
		{
			// Don't bother to try and fetch the extra physics flags if we
			// don't have sim support for them...
			if (!getInstance()->mGotExtraPhysics)
			{
				getInstance()->mExportState = EXPORT_STRUCTURE;
				break;
			}

			struct ff : public LLSelectedNodeFunctor
			{
				virtual bool apply(LLSelectNode* node)
				{
					LLViewerObject* object = node->getObject();
					return object->getPhysicsShapeUnknown();
				}
			} func;

			LLViewerObject* object = LLSelectMgr::getInstance()->getSelection()->getPrimaryObject();
			if (object)
			{
				if (LLSelectMgr::getInstance()->getSelection()->applyToNodes(&func, false))
				{
					getInstance()->mExportState = EXPORT_STRUCTURE;
				}
				else
				{
					LL_DEBUGS("ObjectBackup") << "Nodes physics not yet received, delaying..."
											  << LL_ENDL;
				}
			}
			else
			{
				getInstance()->mExportState = EXPORT_FAILED;
				LLSelectMgr::getInstance()->getSelection()->unref();
			}
			break;
		}

		case EXPORT_STRUCTURE:
		{
			struct ff : public LLSelectedObjectFunctor
			{
				virtual bool apply(LLViewerObject* object)
				{
					bool is_attachment = object->isAttachment();
					object->boostTexturePriority(true);
					LLViewerObject::child_list_t children = object->getChildren();
					children.push_front(object); //push root onto list
					LLSD prim_llsd = LLObjectBackup::getInstance()->primsToLLSD(children, is_attachment);
					LLSD stuff;
					if (is_attachment)
					{
						stuff["root_position"] = object->getPositionEdit().getValue();
						stuff["root_rotation"] = ll_sd_from_quaternion(object->getRotationEdit());
					}
					else
					{
						stuff["root_position"] = object->getPosition().getValue();
						stuff["root_rotation"] = ll_sd_from_quaternion(object->getRotation());
					}
					stuff["group_body"] = prim_llsd;
					getInstance()->mLLSD["data"].append(stuff);
					return true;
				}
			} func;

			getInstance()->mExportState = EXPORT_LLSD;
			LLSelectMgr::getInstance()->getSelection()->applyToRootObjects(&func, false);
			LLSelectMgr::getInstance()->getSelection()->unref();
			break;
		}

		case EXPORT_TEXTURES:
		{
			if (!getInstance()->mCheckNextTexture)
			{
				// The texture is being fetched. Wait till next idle callback.
				return;
			}

			if (getInstance()->mTexturesList.empty())
			{
				getInstance()->mExportState = EXPORT_DONE;
				return;
			}

			// Ok, we got work to do...
			getInstance()->mCheckNextTexture= false;
			getInstance()->exportNextTexture();
			break;
		}

		case EXPORT_LLSD:
		{
			// Create a file stream and write to it
			llofstream export_file(getInstance()->mFileName);
			LLSDSerialize::toPrettyXML(getInstance()->mLLSD, export_file);
			export_file.close();
			getInstance()->mCheckNextTexture = true;
			getInstance()->mExportState = EXPORT_TEXTURES;
			break;
		}

		case EXPORT_DONE:
		{
			gIdleCallbacks.deleteFunction(exportWorker);
			if (LLObjectBackup::getInstance()->mNonExportedTextures == LLObjectBackup::TEXTURE_OK)
			{
				LL_INFOS("ObjectBackup") << "Export successful and complete." << LL_ENDL;
				LLNotificationsUtil::add("ExportSuccessful");
			}
			else
			{
				LL_INFOS("ObjectBackup") << "Export successful but incomplete: some texture(s) not saved." << LL_ENDL;
				std::string reason;
				if (getInstance()->mNonExportedTextures & LLObjectBackup::TEXTURE_BAD_PERM)
				{
					reason += "\nBad permissions/creator.";
				}
				if (getInstance()->mNonExportedTextures & LLObjectBackup::TEXTURE_MISSING)
				{
					reason += "\nMissing texture (retrying after full rezzing might work).";
				}
				if (getInstance()->mNonExportedTextures & LLObjectBackup::TEXTURE_BAD_ENCODING)
				{
					reason += "\nBad texture encoding.";
				}
				if (getInstance()->mNonExportedTextures & LLObjectBackup::TEXTURE_IS_NULL)
				{
					reason += "\nNull texture.";
				}
				if (getInstance()->mNonExportedTextures & LLObjectBackup::TEXTURE_SAVED_FAILED)
				{
					reason += "\nCould not write to disk.";
				}
				LLSD args;
				args["REASON"] = reason;
				LLNotificationsUtil::add("ExportPartial", args);
			}
			getInstance()->close();
			break;
		}

		case EXPORT_FAILED:
		{
			gIdleCallbacks.deleteFunction(exportWorker);
			LL_WARNS("ObjectBackup") << "Export process aborted." << LL_ENDL;
			LLNotificationsUtil::add("ExportFailed");
			LLObjectBackup::getInstance()->close();
			break;
		}
	}
}

LLSD LLObjectBackup::primsToLLSD(LLViewerObject::child_list_t child_list, bool is_attachment)
{
	LLViewerObject* object;
	LLSD llsd;
	char localid[16];
	LLUUID t_id;

	for (LLViewerObject::child_list_t::iterator i = child_list.begin();
		 i != child_list.end(); ++i)
	{
		object = (*i);
		LLUUID id = object->getID();

		LL_INFOS("ObjectBackup") << "Exporting prim " << object->getID().asString() << LL_ENDL;

		// Create an LLSD object that represents this prim. It will be injected
		// in to the overall LLSD tree structure
		LLSD prim_llsd;

		if (!object->isRoot())
		{
			// Parent id
			snprintf(localid, sizeof(localid), "%u",
					 object->getSubParent()->getLocalID());
			prim_llsd["parent"] = localid;
		}

		// Name and description
		LLSelectNode* node = LLSelectMgr::getInstance()->getSelection()->findNode(object);
		if (node)
		{
			prim_llsd["name"] = node->mName;
			prim_llsd["description"] = node->mDescription;
		}

		// Transforms
		if (is_attachment)
		{
			prim_llsd["position"] = object->getPositionEdit().getValue();
			prim_llsd["rotation"] = ll_sd_from_quaternion(object->getRotationEdit());
		}
		else
		{
			prim_llsd["position"] = object->getPosition().getValue();
			prim_llsd["rotation"] = ll_sd_from_quaternion(object->getRotation());
		}
		prim_llsd["scale"] = object->getScale().getValue();

		// Flags
		prim_llsd["phantom"] = object->flagPhantom();		// legacy
		prim_llsd["physical"] = object->flagUsePhysics();	// legacy
		prim_llsd["flags"] = (S32)object->getFlags();		// new way

		// Extra physics flags
		if (mGotExtraPhysics)
		{
			LLSD& physics = prim_llsd["ExtraPhysics"];
			physics["PhysicsShapeType"] = object->getPhysicsShapeType();
			physics["Gravity"] = object->getPhysicsGravity();
			physics["Friction"] = object->getPhysicsFriction();
			physics["Density"] = object->getPhysicsDensity();
			physics["Restitution"] = object->getPhysicsRestitution();
		}

		// Click action
		if (S32 action = object->getClickAction()) // Non-zero
			prim_llsd["clickaction"] = action;

		// Prim material
		prim_llsd["material"] = object->getMaterial();

		// Volume params
		LLVolumeParams params = object->getVolume()->getParams();
		prim_llsd["volume"] = params.asLLSD();

		// Extra paramsb6fab961-af18-77f8-cf08-f021377a7244
		if (object->isFlexible())
		{
			// Flexible
			LLFlexibleObjectData* flex = (LLFlexibleObjectData*)object->getParameterEntry(LLNetworkData::PARAMS_FLEXIBLE);
			prim_llsd["flexible"] = flex->asLLSD();
		}

		if (object->getParameterEntryInUse(LLNetworkData::PARAMS_LIGHT))
		{
			// Light
			LLLightParams* light = (LLLightParams*)object->getParameterEntry(LLNetworkData::PARAMS_LIGHT);
			prim_llsd["light"] = light->asLLSD();
		}
		if (object->getParameterEntryInUse(LLNetworkData::PARAMS_LIGHT_IMAGE))
		{
			// Light image
			LLLightImageParams* light_param = (LLLightImageParams*)object->getParameterEntry(LLNetworkData::PARAMS_LIGHT_IMAGE);
			t_id = validateTextureID(light_param->getLightTexture());
			if (mTexturesList.count(t_id) == 0)
			{
				LL_INFOS("ObjectBackup") << "Found a light texture, adding to list " << t_id << LL_ENDL;
				mTexturesList.insert(t_id);
			}
			prim_llsd["light_texture"] = light_param->asLLSD();
		}

		if (object->getParameterEntryInUse(LLNetworkData::PARAMS_SCULPT))
		{
			// Sculpt
			LLSculptParams* sculpt = (LLSculptParams*)object->getParameterEntry(LLNetworkData::PARAMS_SCULPT);
			prim_llsd["sculpt"] = sculpt->asLLSD();
			if ((sculpt->getSculptType() & LL_SCULPT_TYPE_MASK) != LL_SCULPT_TYPE_MESH)
			{
				LLUUID sculpt_texture = sculpt->getSculptTexture();
				if (sculpt_texture == validateTextureID(sculpt_texture))
				{
					if (mTexturesList.count(sculpt_texture) == 0)
					{
						LL_INFOS("ObjectBackup") << "Found a sculpt texture, adding to list " << sculpt_texture << LL_ENDL;
						mTexturesList.insert(sculpt_texture);
					}
				}
				else
				{
					LL_WARNS("ObjectBackup") << "Incorrect permission to export a sculpt texture." << LL_ENDL;
					getInstance()->mExportState = EXPORT_FAILED;
				}
			}
		}

		// Textures and materials
		LLSD te_llsd;
		LLSD this_te_llsd;
		LLSD te_mat_llsd;
		LLSD this_te_mat_llsd;
		bool has_materials = false;
		for (U8 i = 0, count = object->getNumTEs(); i < count; ++i)
		{
			LLTextureEntry* te = object->getTE(i);
			if (!te) continue;	// Paranoia

			// Normal texture/diffuse map
			t_id = validateTextureID(te->getID());
			this_te_llsd = te->asLLSD();
			this_te_llsd["imageid"] = t_id;
			te_llsd.append(this_te_llsd);
			// Do not export Linden textures even though they don't taint creation.
			if (t_id != LL_TEXTURE_PLYWOOD &&
			    t_id != LL_TEXTURE_BLANK && 
			    t_id != LL_TEXTURE_TRANSPARENT &&
			    t_id != LL_TEXTURE_INVISIBLE &&
			    t_id != LL_TEXTURE_MEDIA)
			{
				if (mTexturesList.count(t_id) == 0)
				{
					mTexturesList.insert(t_id);
				}
			}

			// Materials
			LLMaterial* mat = te->getMaterialParams().get();
			if (mat)
			{
				has_materials = true;
				this_te_mat_llsd = mat->asLLSD();

				t_id = validateTextureID(mat->getNormalID());
				this_te_mat_llsd["NormMap"] = t_id;
				if (mTexturesList.count(t_id) == 0)
				{
					mTexturesList.insert(t_id);
				}

				t_id = validateTextureID(mat->getSpecularID());
				this_te_mat_llsd["SpecMap"] = t_id;
				if (mTexturesList.count(t_id) == 0)
				{
					mTexturesList.insert(t_id);
				}

				te_mat_llsd.append(this_te_mat_llsd);
			}
		}
		prim_llsd["textures"] = te_llsd;
		if (has_materials)
		{
			prim_llsd["materials"] = te_mat_llsd;
		}

		// The keys in the primitive maps do not have to be localids, they can be any
		// string. We simply use localids because they are a unique identifier
		snprintf(localid, sizeof(localid), "%u", object->getLocalID());
		llsd[(const char*)localid] = prim_llsd;
	}

	updateExportNumbers();

	return llsd;
}

void LLObjectBackup::exportNextTexture()
{
	LLUUID id;
	textures_set_t::iterator iter = mTexturesList.begin();
	while (true)
	{
		if (mTexturesList.empty())
		{
			mCheckNextTexture = true;
			LL_INFOS("ObjectBackup") << "Finished exporting textures." << LL_ENDL;
			return;
		}
		if (iter == mTexturesList.end())
		{
			// Not yet ready, wait and re-check at next idle callback...
			mCheckNextTexture = true;
			return;
		}

		id = *iter++;
		if (id.isNull())
		{
			// NULL texture id: just remove and ignore.
			mTexturesList.erase(id);
			LL_DEBUGS("ObjectBackup") << "Null texture UUID found, ignoring."
									  << LL_ENDL;
			continue;
		}

		LLViewerTexture* imagep = LLViewerTextureManager::findTexture(id);
		if (imagep)
		{
			if (imagep->getDiscardLevel() > 0)
			{
				// Boost texture loading
				imagep->setBoostLevel(LLGLTexture::BOOST_PREVIEW);
				LL_DEBUGS("ObjectBackup") << "Boosting texture: " << id
										  << LL_ENDL;
				LLViewerFetchedTexture* tex;
				tex = LLViewerTextureManager::staticCastToFetchedTexture(imagep);
				if (tex && tex->getDesiredDiscardLevel() > 0)
				{
					// Set min discard level to 0
					tex->setMinDiscardLevel(0);
					LL_DEBUGS("ObjectBackup") << "Min discard level set to 0 for texture: "
											  << id << LL_ENDL;
				}
			}
			else
			{
				// Texture is ready !
				break;
			}
		}
		else
		{
			LL_WARNS("ObjectBackup") << "We *DON'T* have the texture " << id.asString() << LL_ENDL;
			mNonExportedTextures |= TEXTURE_MISSING;
			mTexturesList.erase(id);
		}
	}

	mTexturesList.erase(id);

	LL_INFOS("ObjectBackup") << "Requesting texture " << id << LL_ENDL;
	LLImageJ2C* mFormattedImage = new LLImageJ2C;
	CacheReadResponder* responder = new CacheReadResponder(id, mFormattedImage);
  	LLAppViewer::getTextureCache()->readFromCache(id, LLWorkerThread::PRIORITY_HIGH, 0, 999999, responder);
}

void LLObjectBackup::importObject(bool upload)
{
	mRetexture = upload;
	mTexturesList.clear();
	mAssetMap.clear();
	mCurrentAsset.setNull();
	
	mGotExtraPhysics = !gAgent.getRegion()->getCapability("GetObjectPhysicsData").empty();

	setDefaultTextures();

	// Open the file open dialog
	AIFilePicker* filepicker = AIFilePicker::create();
	filepicker->open(FFLOAD_XML, "", "import");
	filepicker->run(boost::bind(&LLObjectBackup::importObject_continued, this, filepicker));
}

void LLObjectBackup::importObject_continued(AIFilePicker* filepicker)
{
	if (!filepicker->hasFilename())
	{
		// User canceled save.
		return;
	}

	std::string file_name = filepicker->getFilename();
	mFolder = gDirUtilp->getDirName(file_name);
	llifstream import_file(file_name);
	LLSDSerialize::fromXML(mLLSD, import_file);
	import_file.close();
	if (!mLLSD.has("data"))
	{
		LLNotificationsUtil::add("InvalidObjectParams");
		close();
		return;
	}

	show(false);

	mAgentPos = gAgent.getPositionAgent();
	mAgentRot = LLQuaternion(gAgent.getAtAxis(), gAgent.getLeftAxis(),
							 gAgent.getUpAxis());

	// Get the texture map
	mCurObject = 1;
	mCurPrim = 1;
	mObjects = mLLSD["data"].size();
	mPrims = 0;
	mRezCount = 0;
	updateImportNumbers();

	for (LLSD::array_const_iterator prim_arr_it = mLLSD["data"].beginArray(),
									prim_arr_end = mLLSD["data"].endArray();
		 prim_arr_it != prim_arr_end; ++prim_arr_it)
	{
		LLSD llsd2 = (*prim_arr_it)["group_body"];

		for (LLSD::map_const_iterator prim_it = llsd2.beginMap(),
									  prim_end = llsd2.endMap();
			 prim_it != prim_end; ++prim_it)
		{
			LLSD prim_llsd = llsd2[prim_it->first];
			if (prim_llsd.has("sculpt"))
			{
				LLSculptParams sculpt;
				sculpt.fromLLSD(prim_llsd["sculpt"]);
				if ((sculpt.getSculptType() & LL_SCULPT_TYPE_MASK) != LL_SCULPT_TYPE_MESH)
				{
					LLUUID orig = sculpt.getSculptTexture();
					if (mTexturesList.count(orig) == 0)
					{
						LL_INFOS("ObjectBackup") << "Found a new SCULPT texture to upload " << orig << LL_ENDL;
						mTexturesList.insert(orig);
					}
				}
			}

			LLSD& te_llsd = prim_llsd.has("textures") ? prim_llsd["textures"] : prim_llsd["texture"]; // Firestorm's format uses singular "texture"

			for (LLSD::array_iterator it = te_llsd.beginArray();
				 it != te_llsd.endArray(); ++it)
			{
				LLSD the_te = *it;
				LLTextureEntry te;
				te.fromLLSD(the_te);

				LLUUID t_id = te.getID();
				// Do not upload the default textures
				if (t_id != LL_TEXTURE_PLYWOOD && t_id != LL_TEXTURE_BLANK && t_id != LL_TEXTURE_INVISIBLE) // Do not upload the default textures
				{
					if (mTexturesList.count(t_id) == 0)
					{
						LL_INFOS("ObjectBackup") << "Found a new texture to upload: "<< t_id << LL_ENDL;
						mTexturesList.insert(t_id);
					}
				}
			}

			if (prim_llsd.has("materials"))
			{
				LLSD mat_llsd = prim_llsd["materials"];
				for (LLSD::array_iterator it = mat_llsd.beginArray();
					 it != mat_llsd.endArray(); ++it)
				{
					LLSD the_mat = *it;
					LLMaterial mat;
					mat.fromLLSD(the_mat);

					LLUUID t_id = mat.getNormalID();
					if (t_id.notNull() && t_id != LL_TEXTURE_PLYWOOD &&
						t_id != LL_TEXTURE_BLANK &&
						t_id != LL_TEXTURE_INVISIBLE)
					{
						if (mTexturesList.count(t_id) == 0)
						{
							LL_INFOS("ObjectBackup") << "Found a new normal map to upload: "
									<< t_id << LL_ENDL;
							mTexturesList.insert(t_id);
						}
					}

					t_id = mat.getSpecularID();
					if (t_id.notNull() && t_id != LL_TEXTURE_PLYWOOD &&
						t_id != LL_TEXTURE_BLANK &&
						t_id != LL_TEXTURE_INVISIBLE)
					{
						if (mTexturesList.count(t_id) == 0)
						{
							LL_INFOS("ObjectBackup")  << "Found a new specular map to upload: "
									<< t_id << LL_ENDL;
							mTexturesList.insert(t_id);
						}
					}
				}
			}
		}
	}

	if (mRetexture)
	{
		uploadNextAsset();
	}
	else
	{
		importFirstObject();
	}
}

LLVector3 LLObjectBackup::offsetAgent(LLVector3 offset)
{
	return offset * mAgentRot + mAgentPos;
}

void LLObjectBackup::rezAgentOffset(LLVector3 offset)
{
	// This will break for a sitting agent
	LLToolPlacer mPlacer;
	mPlacer.setObjectType(LL_PCODE_CUBE);
	mPlacer.placeObject((S32)offset.mV[0], (S32)offset.mV[1], MASK_NONE);
}

void LLObjectBackup::importFirstObject()
{
	mRunning = true;
	show(false);
	mGroupPrimImportIter = mLLSD["data"].beginArray();	
	mRootRootPos = (*mGroupPrimImportIter)["root_position"];
	mObjects = mLLSD["data"].size();
	mCurObject = 1;
	importNextObject();
}

void LLObjectBackup::importNextObject()
{
	mToSelect.clear();
	mRezCount = 0;

	mThisGroup = (*mGroupPrimImportIter)["group_body"];
	mPrimImportIter = mThisGroup.beginMap();

	mCurPrim = 0;
	mPrims = mThisGroup.size();
	updateImportNumbers();

	LLVector3 lgpos = (*mGroupPrimImportIter)["root_position"];
	mGroupOffset = lgpos - mRootRootPos;
	mRootPos = offsetAgent(LLVector3(2.0, 0.0, 0.0));
	mRootRot = ll_quaternion_from_sd((*mGroupPrimImportIter)["root_rotation"]);

	rezAgentOffset(LLVector3(0.0, 2.0, 0.0));
	// Now we must wait for the callback when ViewerObjectList gets the new
	// objects and we have the correct number selected
}

// This function takes a pointer to a viewerobject and applies the prim
// definition that prim_llsd has
void LLObjectBackup::xmlToPrim(LLSD prim_llsd, LLViewerObject* object)
{
	LLUUID id = object->getID();
	mExpectingUpdate = object->getID();
	LLSelectMgr::getInstance()->selectObjectAndFamily(object);

	if (prim_llsd.has("name"))
	{
		LLSelectMgr::getInstance()->selectionSetObjectName(prim_llsd["name"]);
	}

	if (prim_llsd.has("description"))
	{
		LLSelectMgr::getInstance()->selectionSetObjectDescription(prim_llsd["description"]);
	}

	if (prim_llsd.has("material"))
	{
		LLSelectMgr::getInstance()->selectionSetMaterial(prim_llsd["material"].asInteger());
	}

	if (prim_llsd.has("clickaction"))
	{
		LLSelectMgr::getInstance()->selectionSetClickAction(prim_llsd["clickaction"].asInteger());
	}

	if (prim_llsd.has("parent"))
	{
		//we are not the root node.
		LLVector3 pos = prim_llsd["position"];
		LLQuaternion rot = ll_quaternion_from_sd(prim_llsd["rotation"]);
		object->setPositionRegion(pos * mRootRot + mRootPos + mGroupOffset);
		object->setRotation(rot * mRootRot);
	}
	else
	{
		object->setPositionRegion(mRootPos + mGroupOffset);
		LLQuaternion rot=ll_quaternion_from_sd(prim_llsd["rotation"]);
		object->setRotation(rot);
	}

	object->setScale(prim_llsd["scale"]);

	if (prim_llsd.has("flags"))
	{
		U32 flags(prim_llsd["flags"].asInteger());
		object->setFlags(flags, true);
	}
	else	// Kept for backward compatibility
	{
		/*if (prim_llsd.has("shadows"))
			if (prim_llsd["shadows"].asInteger() == 1)
				object->setFlags(FLAGS_CAST_SHADOWS, true);*/

		if (prim_llsd.has("phantom") && prim_llsd["phantom"].asInteger() == 1)
		{
				object->setFlags(FLAGS_PHANTOM, true);
		}

		if (prim_llsd.has("physical") &&
			prim_llsd["physical"].asInteger() == 1)
		{
			object->setFlags(FLAGS_USE_PHYSICS, true);
		}
	}

	if (mGotExtraPhysics && prim_llsd.has("ExtraPhysics"))
	{
		const LLSD& physics = prim_llsd["ExtraPhysics"];
		object->setPhysicsShapeType(physics["PhysicsShapeType"].asInteger());
		F32 gravity = physics.has("Gravity") ? physics["Gravity"].asReal()
											 : physics["GravityMultiplier"].asReal();
		object->setPhysicsGravity(gravity);
		object->setPhysicsFriction(physics["Friction"].asReal());
		object->setPhysicsDensity(physics["Density"].asReal());
		object->setPhysicsRestitution(physics["Restitution"].asReal());
		object->updateFlags(true);
	}

	// Volume params
	LLVolumeParams volume_params = object->getVolume()->getParams();
	volume_params.fromLLSD(prim_llsd["volume"]);
	object->updateVolume(volume_params);

	if (prim_llsd.has("sculpt"))
	{
		LLSculptParams sculpt;
		sculpt.fromLLSD(prim_llsd["sculpt"]);

		// TODO: check if map is valid and only set texture if map is valid and changes

		if (mAssetMap.count(sculpt.getSculptTexture()))
		{
			LLUUID replacement = mAssetMap[sculpt.getSculptTexture()];
			sculpt.setSculptTexture(replacement);
		}

		object->setParameterEntry(LLNetworkData::PARAMS_SCULPT, sculpt, true);
	}
		
	if (prim_llsd.has("light"))
	{
		LLLightParams light;
		light.fromLLSD(prim_llsd["light"]);
		object->setParameterEntry(LLNetworkData::PARAMS_LIGHT, light, true);
	}
	if (prim_llsd.has("light_texture"))
	{
		// Light image
		LLLightImageParams lightimg;
		lightimg.fromLLSD(prim_llsd["light_texture"]);
		object->setParameterEntry(LLNetworkData::PARAMS_LIGHT_IMAGE, lightimg, true);
	}

	if (prim_llsd.has("flexible"))
	{
		LLFlexibleObjectData flex;
		flex.fromLLSD(prim_llsd["flexible"]);
		object->setParameterEntry(LLNetworkData::PARAMS_FLEXIBLE, flex, true);
	}

	// Textures
	LL_INFOS("ObjectBackup") << "Processing textures for prim" << id << LL_ENDL;
	LLSD& te_llsd = prim_llsd.has("textures") ? prim_llsd["textures"] : prim_llsd["texture"]; // Firestorm's format uses singular "texture"
	U8 i = 0;
	for (LLSD::array_iterator it =  te_llsd.beginArray();
		 it != te_llsd.endArray(); ++it)
	{
	    LLSD the_te = *it;
	    LLTextureEntry te;
	    te.fromLLSD(the_te);
		LLUUID t_id = te.getID();
		if (mAssetMap.count(t_id))
		{
			LLUUID replacement = mAssetMap[t_id];
			te.setID(replacement);
		}

	    object->setTE(i++, te);
	}
	LL_INFOS("ObjectBackup") << "Textures done !" << LL_ENDL;

	// Materials
	if (prim_llsd.has("materials"))
	{
		LL_INFOS("ObjectBackup")  << "Processing materials for prim " << id << LL_ENDL;
		te_llsd = prim_llsd["materials"];
		i = 0;
		for (LLSD::array_iterator it = te_llsd.beginArray();
			 it != te_llsd.endArray(); ++it)
		{
		    LLSD the_mat = *it;
			LLMaterialPtr mat = new LLMaterial(the_mat);

			LLUUID t_id = mat->getNormalID();
			if (id.notNull() && mAssetMap.count(t_id))
			{
				LLUUID replacement = mAssetMap[t_id];
				mat->setNormalID(replacement);
			}

			t_id = mat->getSpecularID();
			if (id.notNull() && mAssetMap.count(t_id))
			{
				LLUUID replacement = mAssetMap[t_id];
				mat->setSpecularID(replacement);
			}

			LLMaterialMgr::getInstance()->put(id, i++, *mat);
		}
		LL_INFOS("ObjectBackup") << "Materials done !" << LL_ENDL;
	}

	object->sendRotationUpdate();
	object->sendTEUpdate();	
	object->sendShapeUpdate();
	LLSelectMgr::getInstance()->sendMultipleUpdate(UPD_SCALE | UPD_POSITION);

	LLSelectMgr::getInstance()->deselectAll();
}

// This is fired when the update packet is processed so we know the prim settings have stuck
void LLObjectBackup::primUpdate(LLViewerObject* object)
{
	if (!mRunning || (object != NULL && object->mID != mExpectingUpdate))
	{
			return;
	}

	++mCurPrim;
	updateImportNumbers();
	++mPrimImportIter;

	LLUUID x;
	mExpectingUpdate = x.null;

	if (mPrimImportIter == mThisGroup.endMap())
	{
		LL_INFOS("ObjectBackup") << "Trying to link" << LL_ENDL;

		if (mToSelect.size() > 1)
		{
			std::reverse(mToSelect.begin(), mToSelect.end());
			// Now link
			LLSelectMgr::getInstance()->deselectAll();
			LLSelectMgr::getInstance()->selectObjectAndFamily(mToSelect, true);
			LLSelectMgr::getInstance()->sendLink();
			LLViewerObject* root = mToSelect.back();
			root->setRotation(mRootRot);
		}

		++mCurObject;
		++mGroupPrimImportIter;
		if (mGroupPrimImportIter != mLLSD["data"].endArray())
		{
			importNextObject();
			return;
		}

		mRunning = false;
		close();
		return;
	}

	LLSD prim_llsd = mThisGroup[mPrimImportIter->first];

	if (mToSelect.empty())
	{
		LL_WARNS("ObjectBackup") << "error: ran out of objects to mod." << LL_ENDL;
		mRunning = false;
		close();
		return;
	}

	if (mPrimImportIter != mThisGroup.endMap())
	{
		//rezAgentOffset(LLVector3(1.0, 0.0, 0.0));
		LLSD prim_llsd = mThisGroup[mPrimImportIter->first];
		++mProcessIter;
		xmlToPrim(prim_llsd, *mProcessIter);	
	}
}

// Callback when we rez a new object when the importer is running.
bool LLObjectBackup::newPrim(LLViewerObject* pobject)
{
	if (mRunning)
	{
		++mRezCount;
		mToSelect.push_back(pobject);
		updateImportNumbers();
		++mPrimImportIter;

		pobject->setPosition(offsetAgent(LLVector3(0.0, 1.0, 0.0)));
		LLSelectMgr::getInstance()->sendMultipleUpdate(UPD_POSITION);

		if (mPrimImportIter != mThisGroup.endMap())
		{
			rezAgentOffset(LLVector3(1.0, 0.0 ,0.0));
		}
		else
		{
			LL_INFOS("ObjectBackup") << "All prims rezzed, moving to build stage" << LL_ENDL;
			// Deselecting is required to ensure that the first child prim in
			// the link set (which is also the last rezzed prim and thus
			// currently selected) will be properly renamed and desced.
			LLSelectMgr::getInstance()->deselectAll();
			mPrimImportIter = mThisGroup.beginMap();
			LLSD prim_llsd = mThisGroup[mPrimImportIter->first];
			mProcessIter = mToSelect.begin();
			xmlToPrim(prim_llsd, *mProcessIter);
		}
	}
	return true;
}

void LLObjectBackup::updateMap(LLUUID uploaded_asset)
{
	if (mCurrentAsset.notNull())
	{
		LL_INFOS("ObjectBackup") << "Mapping " << mCurrentAsset << " to " << uploaded_asset << LL_ENDL;

		mAssetMap[mCurrentAsset] = uploaded_asset;
	}
}

void myupload_new_resource(const LLTransactionID &tid,
						 LLAssetType::EType asset_type,
						 std::string name,
						 std::string desc,
						 S32 compression_info,
						 LLFolderType::EType destination_folder_type,
						 LLInventoryType::EType inv_type,
						 U32 next_owner_perm,
						 const std::string& display_name,
						 LLAssetStorage::LLStoreAssetCallback callback,
						 void *userdata)
{
	if (gDisconnected)
	{
		return;
	}

	LLAssetID uuid = tid.makeAssetID(gAgent.getSecureSessionID());	

	// At this point, we're ready for the upload.
	std::string upload_message = "Uploading...\n\n";
	upload_message.append(display_name);
	LLUploadDialog::modalUploadDialog(upload_message);

	std::string url = gAgent.getRegion()->getCapability("NewFileAgentInventory");
	if (!url.empty())
	{
		LLSD body;
		body["folder_id"] = gInventory.findCategoryUUIDForType(destination_folder_type == LLFolderType::FT_NONE ?
															   LLFolderType::assetTypeToFolderType(asset_type) :
															   destination_folder_type);
		body["asset_type"] = LLAssetType::lookup(asset_type);
		body["inventory_type"] = LLInventoryType::lookup(inv_type);
		body["name"] = name;
		body["description"] = desc;

		std::ostringstream llsdxml;
		LLSDSerialize::toXML(body, llsdxml);
		LL_DEBUGS("ObjectBackup") << "posting body to capability: " << llsdxml.str() << LL_ENDL;
		LLHTTPClient::post(url, body, new importResponder(body, uuid, asset_type));
	}
	else
	{
		LL_INFOS("ObjectBackup") << "NewAgentInventory capability not found. Can't upload !" << LL_ENDL;	
	}
}

void LLObjectBackup::uploadNextAsset()
{
	if (mTexturesList.empty())
	{
		LL_INFOS("ObjectBackup") << "Texture list is empty, moving to rez stage." << LL_ENDL;
		mCurrentAsset.setNull();
		importFirstObject();
		return;
	}

	updateImportNumbers();

	textures_set_t::iterator iter = mTexturesList.begin();
	LLUUID id = *iter;
	mTexturesList.erase(iter);

	LL_INFOS("ObjectBackup") << "Got texture ID " << id << ": trying to upload..." << LL_ENDL;

	mCurrentAsset = id;
	std::string struid;
	id.toString(struid);
	std::string filename = mFolder + "//" + struid;
	LLAssetID uuid;
	LLTransactionID tid;

	// generate a new transaction ID for this asset
	tid.generate();
	uuid = tid.makeAssetID(gAgent.getSecureSessionID());

	S32 file_size;
	LLAPRFile outfile(filename, LL_APR_RB, &file_size);
	if (outfile.getFileHandle())
	{
		const S32 buf_size = 65536;	
		U8 copy_buf[buf_size];
		LLVFile file(gVFS, uuid,  LLAssetType::AT_TEXTURE, LLVFile::WRITE);
		file.setMaxSize(file_size);
		while ((file_size = outfile.read(copy_buf, buf_size)))
		{
			file.write(copy_buf, file_size);
		}
		outfile.close();
	}
	else
	{
		LL_WARNS("ObjectBackup") << "Unable to access output file " << filename << LL_ENDL;
		uploadNextAsset();
		return;
	}

	 myupload_new_resource(tid, LLAssetType::AT_TEXTURE, struid, struid, 0,
		LLFolderType::FT_TEXTURE,
		LLInventoryType::defaultForAssetType(LLAssetType::AT_TEXTURE),
		0x0, "Uploaded texture", NULL, NULL);
}
