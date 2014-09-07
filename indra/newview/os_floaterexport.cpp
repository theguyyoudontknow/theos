/** 
* @file os_floaterexport.h
* Simms - 2014
* $/LicenseInfo$
*/
 
#include "llviewerprecompiledheaders.h"
#include "os_floaterexport.h"
#include "lluictrlfactory.h"
#include "llsdutil.h"
#include "llsdserialize.h"
#include "llselectmgr.h"
#include "llscrolllistctrl.h"
#include "llchat.h"
#include "llfloaterchat.h"
#include "statemachine/aifilepicker.h"
#include "llagent.h"
#include "llvoavatar.h"
#include "llvoavatarself.h"
#include "llavatarappearancedefines.h"
#include "os_importobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llwearabletype.h"
#include "llwindow.h"
#include "llviewertexturelist.h"
#include "lltexturecache.h"
#include "llimage.h"
#include "llappviewer.h"
#include "llsdutil_math.h"
#include "llimagej2c.h"
std::vector<OSFloaterExport*> OSFloaterExport::instances;

using namespace LLAvatarAppearanceDefines;

class CacheReadResponder : public LLTextureCache::ReadResponder
{
public:
CacheReadResponder(const LLUUID& id, const std::string& filename)
: mID(id)
{
	mFormattedImage = new LLImageJ2C;
	setImage(mFormattedImage);
	mFilename = filename;
}
void setData(U8* data, S32 datasize, S32 imagesize, S32 imageformat, BOOL imagelocal)
{
	if(imageformat==IMG_CODEC_TGA && mFormattedImage->getCodec()==IMG_CODEC_J2C)
	{
		llwarns<<"problem downloading tga"<<llendl;
		mFormattedImage=NULL;
		mImageSize=0;
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
		mFormattedImage->setData(data,datasize);
	}
	mImageSize = imagesize;
	mImageLocal = imagelocal;
}

virtual void completed(bool success)
{
	if(success && (mFormattedImage.notNull()) && mImageSize>0)
	{

		llinfos << "SUCCESS getting texture "<<mID<< llendl;

		llinfos << "Saving to "<< mFilename<<llendl;

		if(!mFormattedImage->save(mFilename))
		{
			llinfos << "FAIL saving texture "<<mID<< llendl;
		}

	}
	else
	{
		if(!success)
			llwarns << "FAIL NOT SUCCESSFUL getting texture "<<mID<< llendl;
		if(mFormattedImage.isNull())
			llwarns << "FAIL image is NULL "<<mID<< llendl;
	}
}
private:
	LLPointer<LLImageFormatted> mFormattedImage;
	LLUUID mID;
	std::string mFilename;
};


OSExportable::OSExportable(LLViewerObject* object, std::string name, std::map<U32,std::pair<std::string, std::string> >& primNameMap)
:	mObject(object),
	mType(EXPORTABLE_OBJECT),
	mPrimNameMap(&primNameMap)
{
}

OSExportable::OSExportable(LLVOAvatar* avatar, LLWearableType::EType type, std::map<U32,std::pair<std::string, std::string> >& primNameMap)
:	mAvatar(avatar),
	mType(EXPORTABLE_WEARABLE),
	mWearableType(type),
	mPrimNameMap(&primNameMap)
{
}

LLSD OSExportable::asLLSD()
{
	if(mType == EXPORTABLE_OBJECT)
	{
		std::list<LLViewerObject*> prims;

		prims.push_back(mObject);

		LLViewerObject::child_list_t child_list = mObject->getChildren();
		for (LLViewerObject::child_list_t::iterator i = child_list.begin(); i != child_list.end(); ++i)
		{
			LLViewerObject* child = *i;
			if(child->getPCode() < LL_PCODE_APP)
			{
				prims.push_back(child);
			}
		}

		LLSD llsd;

		std::list<LLViewerObject*>::iterator prim_iter = prims.begin();
		std::list<LLViewerObject*>::iterator prims_end = prims.end();
		for( ; prim_iter != prims_end; ++prim_iter)
		{
			LLViewerObject* object = (*prim_iter);

			LLSD prim_llsd;

			prim_llsd["type"] = "prim";

			if (!object->isRoot())
			{
				if(!object->getSubParent()->isAvatar())
				{
					// Parent id
					prim_llsd["parent"] = llformat("%d", object->getSubParent()->getLocalID());
				}
			}
			if(object->getSubParent())
			{
				if(object->getSubParent()->isAvatar())
				{
					// attachment-specific
					U8 state = object->getState();
					S32 attachpt = ((S32)((((U8)state & AGENT_ATTACH_MASK) >> 4) | (((U8)state & ~AGENT_ATTACH_MASK) << 4)));
					prim_llsd["attach"] = attachpt;
				}
			}

			// Transforms
			prim_llsd["position"] = object->getPosition().getValue();
			prim_llsd["scale"] = object->getScale().getValue();
			prim_llsd["rotation"] = ll_sd_from_quaternion(object->getRotation());

			// Flags
			//prim_llsd["shadows"] = object->flagCastShadows();
			prim_llsd["phantom"] = object->flagPhantom();
			prim_llsd["physical"] = object->flagUsePhysics();

			// Volume params
			LLVolumeParams params = object->getVolume()->getParams();
			prim_llsd["volume"] = params.asLLSD();

			// Extra params
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
			if (object->getParameterEntryInUse(LLNetworkData::PARAMS_SCULPT))
			{
				// Sculpt
				LLSculptParams* sculpt = (LLSculptParams*)object->getParameterEntry(LLNetworkData::PARAMS_SCULPT);
				prim_llsd["sculpt"] = sculpt->asLLSD();
			}

			// Textures
			LLSD te_llsd;
			U8 te_count = object->getNumTEs();
			for (U8 i = 0; i < te_count; i++)
			{
				te_llsd.append(object->getTE(i)->asLLSD());
			}
			prim_llsd["textures"] = te_llsd;

			std::map<U32,std::pair<std::string, std::string> >::iterator pos = (*mPrimNameMap).find(object->getLocalID());
			if(pos != (*mPrimNameMap).end())
			{
				prim_llsd["name"] = (*mPrimNameMap)[object->getLocalID()].first;
				prim_llsd["description"] = (*mPrimNameMap)[object->getLocalID()].second;
			}

			llsd[llformat("%d", object->getLocalID())] = prim_llsd;
		}
		
		return llsd;
	}
	else if(mType == EXPORTABLE_WEARABLE)
	{
		LLSD llsd; // pointless map with single key/value

		LLSD item_sd; // map for wearable

		item_sd["type"] = "wearable";

		S32 type_s32 = (S32)mWearableType;
		std::string wearable_name = LLWearableType::getTypeName( mWearableType );

		item_sd["name"] = mAvatar->getFullname() + " " + wearable_name;
		item_sd["wearabletype"] = type_s32;

		LLSD param_map;

		for( LLVisualParam* param = mAvatar->getFirstVisualParam(); param; param = mAvatar->getNextVisualParam() )
		{
			LLViewerVisualParam* viewer_param = (LLViewerVisualParam*)param;
			if( (viewer_param->getWearableType() == type_s32) && 
				(viewer_param->getGroup() == VISUAL_PARAM_GROUP_TWEAKABLE) )
			{
				param_map[llformat("%d", viewer_param->getID())] = viewer_param->getWeight();
			}
		}

		item_sd["params"] = param_map;

		LLSD textures_map;

		for( S32 te = 0; te < LLAvatarAppearanceDefines::TEX_NUM_INDICES; te++ )
		{
			if( LLAvatarAppearanceDictionary::getTEWearableType( (LLAvatarAppearanceDefines::ETextureIndex)te ) == mWearableType )
			{
				LLViewerTexture* te_image = mAvatar->getTEImage( te );
				if( te_image )
				{
					textures_map[llformat("%d", te)] = te_image->getID();
				}
			}
		}

		item_sd["textures"] = textures_map;

		// Generate a unique ID for it...
		LLUUID myid;
		myid.generate();

		llsd[myid.asString()] = item_sd;

		return llsd;
	}
	return LLSD();
}

OSFloaterExport::OSFloaterExport(const LLSD&) 
: LLFloater(std::string("Export List")),
	mObjectID(LLUUID::null),
	mIsAvatar(false),
	mDirty(true)
{
	mCommitCallbackRegistrar.add("Export.SelectAll",	boost::bind(&OSFloaterExport::onClickSelectAll, this));
	mCommitCallbackRegistrar.add("Export.SelectObjects",	boost::bind(&OSFloaterExport::onClickSelectObjects, this));
	mCommitCallbackRegistrar.add("Export.SelectWearales",	boost::bind(&OSFloaterExport::onClickSelectWearables, this));
	mCommitCallbackRegistrar.add("Export.SaveAs",	boost::bind(&OSFloaterExport::onClickSaveAs, this));
	mCommitCallbackRegistrar.add("Export.Copy",	boost::bind(&OSFloaterExport::onClickMakeCopy, this));
	LLUICtrlFactory::getInstance()->buildFloater(this, "os_floater_export.xml", NULL, false);
}

OSFloaterExport::~OSFloaterExport(void)
{
	LLSelectMgr::getInstance()->deselectAll();
	
	std::vector<OSFloaterExport*>::iterator pos = std::find(OSFloaterExport::instances.begin(), OSFloaterExport::instances.end(), this);
	if(pos != OSFloaterExport::instances.end())
	{
		OSFloaterExport::instances.erase(pos);
	}
}

BOOL OSFloaterExport::postBuild()
{
	mObjectID = LLSelectMgr::getInstance()->getSelection()->getPrimaryObject()->getID();
	mExportList = getChild<LLScrollListCtrl>("export_list");
	if (mObjectID.isNull())
	{
		mIsAvatar = false;
		LLSelectMgr::getInstance()->mUpdateSignal.connect(boost::bind(&OSFloaterExport::updateSelection, this));
	}
	else
	{
		LLVOAvatar* avatarp = gObjectList.findAvatar(mObjectID);

		if (avatarp)
		{
			mIsAvatar = true;
		}
		else
		{
			mIsAvatar = false;
			LLSelectMgr::getInstance()->mUpdateSignal.connect(boost::bind(&OSFloaterExport::updateSelection, this));
		}
	}

	return TRUE;
}

void OSFloaterExport::onOpen()
{
	LLObjectSelectionHandle object_selection = LLSelectMgr::getInstance()->getSelection();
	if (!mIsAvatar)
	{
		if(!(object_selection->getPrimaryObject()))
		{
			close();
			return;
		}
		mObjectSelection = LLSelectMgr::getInstance()->getEditSelection();
	}
	dirty();
}

void OSFloaterExport::updateSelection()
{
	LLObjectSelectionHandle object_selection = LLSelectMgr::getInstance()->getSelection();
	LLSelectNode* node = object_selection->getFirstRootNode();

	if (node && !node->mValid)
	{
		return;
	}

	mObjectSelection = object_selection;
	dirty();
}

void OSFloaterExport::dirty()
{
	mDirty = true;
}

void OSFloaterExport::draw()
{
	if (mDirty)
	{
		refresh();
		mDirty = false;
	}
	LLFloater::draw();
}

void OSFloaterExport::refresh()
{	
	if (mIsAvatar)
	{
		LLVOAvatar* avatarp = gObjectList.findAvatar(mObjectID);
		if (!avatarp) return;
		if(!avatars[avatarp]) 
		{
			avatars[avatarp] = true;
			updateAvatarList();
		}
		else
		{
			addObjectStuff();
		}
	}
	else
	{
		addObjectStuff();
	}
}

void OSFloaterExport::updateAvatarList()
{
	LLSelectMgr::getInstance()->deselectAll();
	std::map<LLViewerObject*, bool>::iterator avatar_iter = avatars.begin();
	std::map<LLViewerObject*, bool>::iterator avatars_end = avatars.end();
	for( ; avatar_iter != avatars_end; avatar_iter++)
	{
		LLViewerObject* avatar = (*avatar_iter).first;
		addAvatarStuff((LLVOAvatar*)avatar);
	}
}

void OSFloaterExport::addAvatarStuff(LLVOAvatar* avatarp)
{
	// Add Wearables
	for( S32 type_itr = 0; type_itr < LLWearableType::WT_COUNT; type_itr++ )
	{
		const LLWearableType::EType type = (LLWearableType::EType)type_itr;
		bool exists = false;
		if(type == LLWearableType::WT_SHAPE)
		{
			exists = true;
		}
		else if (type == LLWearableType::WT_ALPHA || type == LLWearableType::WT_TATTOO) //alpha layers and tattos are unsupported for now
		{
			continue;
		}
		else
		{
			for( S32 te = 0; te < LLAvatarAppearanceDefines::TEX_NUM_INDICES; te++ )
			{
				if( LLAvatarAppearanceDictionary::getTEWearableType( (LLAvatarAppearanceDefines::ETextureIndex)te ) == type )
				{
					LLViewerTexture* te_image = avatarp->getTEImage( te );
					if( te_image->getID() != IMG_DEFAULT_AVATAR)
					{
						exists = true;
						break;
					}
				}
			}
		}
		if(exists)
		{
			LLUUID uuid = avatarp->getID();
			std::string wearable_name = LLWearableType::getTypeName( type );
			std::string name = avatarp->getFullname() + " " + wearable_name;

			LLUUID myid;
			myid.generate();

			LLSD element;
			element["id"] = myid;

			LLSD& check_column = element["columns"][LIST_CHECKED];
			check_column["column"] = "checked";
			check_column["type"] = "checkbox";
			check_column["value"] = false;

			LLSD& type_column = element["columns"][LIST_TYPE];
			type_column["column"] = "type";
			type_column["type"] = "icon";
			type_column["value"] = "inv_item_" + wearable_name + ".tga";

			LLSD& name_column = element["columns"][LIST_NAME];
			name_column["column"] = "name";
			name_column["value"] = name;

			LLSD& avatarid_column = element["columns"][LIST_AVATARID];
			avatarid_column["column"] = "avatarid";
			avatarid_column["value"] = uuid;

			OSExportable* exportable = new OSExportable(avatarp, type, mPrimNameMap);

			mExportables[myid] = exportable->asLLSD();

			mExportList->addElement(element, ADD_BOTTOM);
		}
	}
	// Add Attachments
	mObjectSelection = LLSelectMgr::getInstance()->getEditSelection();
	for (LLVOAvatar::attachment_map_t::iterator iter = avatarp->mAttachmentPoints.begin();
		 iter != avatarp->mAttachmentPoints.end(); )
	{
		LLVOAvatar::attachment_map_t::iterator curiter = iter++;

		LLViewerJointAttachment *attachment = curiter->second;
		if (attachment)
		{
			for (LLViewerJointAttachment::attachedobjs_vec_t::const_iterator attachment_iter = attachment->mAttachedObjects.begin();
				 attachment_iter != attachment->mAttachedObjects.end();
				 ++attachment_iter)
			{
				LLViewerObject* attached_object = (*attachment_iter);
				if (attached_object)
				{
					LLSelectMgr::getInstance()->selectObjectAndFamily(attached_object);
				}
			}
		}
	}

	LLSelectMgr::getInstance()->mUpdateSignal.connect(boost::bind(&OSFloaterExport::updateSelection, this));
}

void OSFloaterExport::addObjectStuff()
{
	for (LLObjectSelection::valid_iterator iter = mObjectSelection->valid_begin(); iter != mObjectSelection->valid_end(); iter++)
	{
		LLSelectNode* nodep = *iter;
		LLViewerObject* objectp = nodep->getObject();
		U32 localid = objectp->getLocalID();
		std::string name = nodep->mName;
		mPrimNameMap[localid] = std::pair<std::string, std::string>(name, nodep->mDescription);
		std::string objectp_id = llformat("%d", objectp->getLocalID());

		if (nodep->mCreationDate == 0) continue;
	
		if(mExportList->getItemIndex(objectp->getID()) == -1)
		{
			bool is_attachment = false;
			bool is_root = true;
			LLViewerObject* parentp = objectp->getSubParent();
			if(parentp)
			{
				if(!parentp->isAvatar())
				// parent must be a prim.
					is_root = false;
				else
				// parent is an avatar.
					is_attachment = true;
			}

			bool is_prim = true;
			if(objectp->getPCode() >= LL_PCODE_APP)
				is_prim = false;
			
			if(is_root && is_prim)
			{
				LLSD element;
				element["id"] = objectp->getID();

				LLSD& check_column = element["columns"][LIST_CHECKED];
				check_column["column"] = "checked";
				check_column["type"] = "checkbox";
				check_column["value"] = !mIsAvatar;

				LLSD& type_column = element["columns"][LIST_TYPE];
				type_column["column"] = "type";
				type_column["type"] = "icon";
				type_column["value"] = "inv_item_object.tga";

				LLSD& name_column = element["columns"][LIST_NAME];
				name_column["column"] = "name";
				if(is_attachment)
					name_column["value"] = nodep->mName + " (worn on " + utf8str_tolower(objectp->getAttachmentPointName()) + ")";
				else
					name_column["value"] = nodep->mName;

				LLSD& avatarid_column = element["columns"][LIST_AVATARID];
				avatarid_column["column"] = "avatarid";
				if(is_attachment)
					avatarid_column["value"] = parentp->getID();
				else
					avatarid_column["value"] = LLUUID::null;

				OSExportable* exportable = new OSExportable(objectp, nodep->mName, mPrimNameMap);
				mExportables[objectp->getID()] = exportable->asLLSD();

				mExportList->addElement(element, ADD_BOTTOM);

				addToPrimList(objectp);
			}
		}
	}

	updateNamesProgress();
}

//static
void OSFloaterExport::onClickSelectAll()
{
	std::vector<LLScrollListItem*> items = mExportList->getAllData();
	std::vector<LLScrollListItem*>::iterator item_iter = items.begin();
	std::vector<LLScrollListItem*>::iterator items_end = items.end();
	bool new_value = !((*item_iter)->getColumn(LIST_CHECKED)->getValue());
	for( ; item_iter != items_end; ++item_iter)
	{
		LLScrollListItem* item = (*item_iter);
		item->getColumn(LIST_CHECKED)->setValue(new_value);
	}
}

//static
void OSFloaterExport::onClickSelectObjects()
{
	std::vector<LLScrollListItem*> items = mExportList->getAllData();
	std::vector<LLScrollListItem*>::iterator item_iter = items.begin();
	std::vector<LLScrollListItem*>::iterator items_end = items.end();
	bool new_value = false;
	for( ; item_iter != items_end; ++item_iter)
	{
		if(((*item_iter)->getColumn(LIST_TYPE)->getValue()).asString() == "inv_item_object.tga")
		{
			new_value = !((*item_iter)->getColumn(LIST_CHECKED)->getValue());
			break;
		}
	}
	for(item_iter = items.begin(); item_iter != items_end; ++item_iter)
	{
		if(((*item_iter)->getColumn(LIST_TYPE)->getValue()).asString() == "inv_item_object.tga")
		{
			LLScrollListItem* item = (*item_iter);
			item->getColumn(LIST_CHECKED)->setValue(new_value);
		}
	}
}

//static
void OSFloaterExport::onClickSelectWearables()
{
	std::vector<LLScrollListItem*> items = mExportList->getAllData();
	std::vector<LLScrollListItem*>::iterator item_iter = items.begin();
	std::vector<LLScrollListItem*>::iterator items_end = items.end();
	bool new_value = false;
	for( ; item_iter != items_end; ++item_iter)
	{
		if(((*item_iter)->getColumn(LIST_TYPE)->getValue()).asString() != "inv_item_object.tga")
		{
			new_value = !((*item_iter)->getColumn(LIST_CHECKED)->getValue());
			break;
		}
	}
	for(item_iter = items.begin(); item_iter != items_end; ++item_iter)
	{
		if(((*item_iter)->getColumn(LIST_TYPE)->getValue()).asString() != "inv_item_object.tga")
		{
			LLScrollListItem* item = (*item_iter);
			item->getColumn(LIST_CHECKED)->setValue(new_value);
		}
	}
}

LLSD OSFloaterExport::getLLSD()
{
	std::vector<LLScrollListItem*> items = mExportList->getAllData();
	LLSD sd;
	std::vector<LLScrollListItem*>::iterator item_iter = items.begin();
	std::vector<LLScrollListItem*>::iterator items_end = items.end();
	for( ; item_iter != items_end; ++item_iter)
	{
		LLScrollListItem* item = (*item_iter);
		if(item->getColumn(LIST_CHECKED)->getValue())
		{
			LLSD item_sd = mExportables[item->getUUID()];
			LLSD::map_iterator map_iter = item_sd.beginMap();
			LLSD::map_iterator map_end = item_sd.endMap();
			for( ; map_iter != map_end; ++map_iter)
			{
				std::string key((*map_iter).first);
				LLSD data = (*map_iter).second;
				// copy it...
				sd[key] = data;
			}
		}
	}
	return sd;
}
/*static*/
void OSFloaterExport::onClickSaveAs_Callback(OSFloaterExport* floater, AIFilePicker* filepicker)
{
	if(!filepicker->hasFilename())
	{
		// User canceled save.
		return;
	}
	std::string file_name = filepicker->getFilename();
	std::string path = file_name.substr(0,file_name.find_last_of(".")) + "_assets";
	BOOL download_texture = floater->childGetValue("download_textures");
	LLSD sd = floater->getLLSD();

	if(download_texture)
	{
		if(!LLFile::isdir(path))
		{
			LLFile::mkdir(path);
		}else
		{
			U32 response = OSMessageBox("Directory "+path+" already exists, would you like to continue and override files?", "Directory Already Exists", OSMB_YESNO);
			if(response)
			{
				return;
			}
		}
		path.append(gDirUtilp->getDirDelimiter()); //lets add the Delimiter now
	}
	// set correct names within llsd and download textures
	LLSD::map_iterator map_iter = sd.beginMap();
	LLSD::map_iterator map_end = sd.endMap();
	std::list<LLUUID> textures;

	for( ; map_iter != map_end; ++map_iter)
	{
		std::istringstream keystr((*map_iter).first);
		U32 key;
		keystr >> key;
		LLSD item = (*map_iter).second;
		if(item["type"].asString() == "prim")
		{
			std::string name = floater->mPrimNameMap[key].first;
			item["name"] = name;
			item["description"] = floater->mPrimNameMap[key].second;
			// I don't understand C++ :(
			sd[(*map_iter).first] = item;

			if(download_texture)
			{
				//textures
				LLSD::array_iterator tex_iter = item["textures"].beginArray();
				for( ; tex_iter != item["textures"].endArray(); ++tex_iter)
				{
					textures.push_back((*tex_iter)["imageid"].asUUID());
				}
				if(item.has("sculpt"))
				{
					textures.push_back(item["sculpt"]["texture"].asUUID());
				}
			}
		}
		else if(download_texture && item["type"].asString() == "wearable")
		{
			LLSD::map_iterator tex_iter = item["textures"].beginMap();
			for( ; tex_iter != item["textures"].endMap(); ++tex_iter)
			{
				textures.push_back((*tex_iter).second.asUUID());
			}
		}
	}
	if(download_texture)
	{
		textures.unique();
		while(!textures.empty())
		{
			llinfos << "Requesting texture " << textures.front().asString() << llendl;
			LLViewerTexture* img = gTextureList.findImage(textures.front());
		    img->setBoostLevel(LLViewerTexture::BOOST_MAX_LEVEL);

		    CacheReadResponder* responder = new CacheReadResponder(textures.front(), std::string(path + textures.front().asString() + ".j2c"));
			LLAppViewer::getTextureCache()->readFromCache(textures.front(),LLWorkerThread::PRIORITY_HIGH,0,999999,responder);
			textures.pop_front();
		}
	}

	llofstream export_file(file_name);
	LLSDSerialize::toPrettyXML(sd, export_file);
	export_file.close();

	std::string msg = "Saved ";
	msg.append(file_name);
	if(download_texture) msg.append(" (Content might take some time to download)");
	LLChat chat(msg);
	LLFloaterChat::addChat(chat);

	floater->close();
}
//static
void OSFloaterExport::onClickSaveAs()
{
	LLSD sd = getLLSD();

	if(sd.size())
	{
		std::string default_filename = "untitled";

		// count the number of selected items
		std::vector<LLScrollListItem*> items = mExportList->getAllData();
		int item_count = 0;
		LLUUID avatarid = (*(items.begin()))->getColumn(LIST_AVATARID)->getValue().asUUID();
		bool all_same_avatarid = true;
		std::vector<LLScrollListItem*>::iterator item_iter = items.begin();
		std::vector<LLScrollListItem*>::iterator items_end = items.end();
		for( ; item_iter != items_end; ++item_iter)
		{
			LLScrollListItem* item = (*item_iter);
			if(item->getColumn(LIST_CHECKED)->getValue())
			{
				item_count++;
				if(item->getColumn(LIST_AVATARID)->getValue().asUUID() != avatarid)
				{
					all_same_avatarid = false;
				}
			}
		}

		if(item_count == 1)
		{
			// Exporting one item?  Use its name for the filename.
			// But make sure it's a root!
			LLSD target = (*(sd.beginMap())).second;
			if(target.has("parent"))
			{
				std::string parentid = target["parent"].asString();
				target = sd[parentid];
			}
			if(target.has("name"))
			{
				if(target["name"].asString().length() > 0)
				{
					default_filename = target["name"].asString();
				}
			}
		}
		else
		{
			// Multiple items?
			// If they're all part of the same avatar, use the avatar's name as filename.
			if(all_same_avatarid)
			{
				std::string fullname;
				if(gCacheName->getFullName(avatarid, fullname))
				{
					default_filename = fullname;
				}
			}
		}
		AIFilePicker* filepicker = AIFilePicker::create();
		filepicker->open(LLDir::getScrubbedFileName(default_filename + ".xml"), FFSAVE_XML);
		filepicker->run(boost::bind(&OSFloaterExport::onClickSaveAs_Callback, this, filepicker));
	}
	else
	{
		std::string msg = "No exportable items selected";
		LLChat chat(msg);
		LLFloaterChat::addChat(chat);
		return;
	}	
}

//static
void OSFloaterExport::onClickMakeCopy()
{
	LLSD sd = getLLSD();

	if(sd.size())
	{
		LLXmlImport::import(new LLXmlImportOptions(sd));
	}
	else
	{
		std::string msg = "No copyable items selected";
		LLChat chat(msg);
		LLFloaterChat::addChat(chat);
		return;
	}
	
	close();
}

void OSFloaterExport::addToPrimList(LLViewerObject* object)
{
	mPrimList.push_back(object->getLocalID());
	LLViewerObject::child_list_t child_list = object->getChildren();
	for (LLViewerObject::child_list_t::iterator i = child_list.begin(); i != child_list.end(); ++i)
	{
		LLViewerObject* child = *i;
		if(child->getPCode() < LL_PCODE_APP)
		{
			mPrimList.push_back(child->getLocalID());
		}
	}
}

void OSFloaterExport::updateNamesProgress()
{
	childSetText("names_progress_text", llformat("Names retrieved: %d of %d", mPrimNameMap.size(), mPrimList.size()));

	if( mPrimNameMap.size()==mPrimList.size())
		LLSelectMgr::getInstance()->deselectAll();
}

void OSFloaterExport::receivePrimName(LLViewerObject* object, std::string name, std::string desc)
{
	LLUUID fullid = object->getID();
	U32 localid = object->getLocalID();
	if(std::find(mPrimList.begin(), mPrimList.end(), localid) != mPrimList.end())
	{
		mPrimNameMap[localid] = std::pair<std::string, std::string>(name, desc);
		S32 item_index = mExportList->getItemIndex(fullid);
		if(item_index != -1)
		{
			std::vector<LLScrollListItem*> items = mExportList->getAllData();
			std::vector<LLScrollListItem*>::iterator iter = items.begin();
			std::vector<LLScrollListItem*>::iterator end = items.end();
			for( ; iter != end; ++iter)
			{
				if((*iter)->getUUID() == fullid)
				{
					(*iter)->getColumn(LIST_NAME)->setValue(name + " (worn on " + utf8str_tolower(object->getAttachmentPointName()) + ")");
					break;
				}
			}
		}
		updateNamesProgress();
	}
}

// static
void OSFloaterExport::receiveObjectProperties(LLUUID fullid, std::string name, std::string desc)
{
	if(!OSFloaterExport::instances.empty())
	{
		LLViewerObject* object = gObjectList.findObject(fullid);
		std::vector<OSFloaterExport*>::iterator iter = OSFloaterExport::instances.begin();
		std::vector<OSFloaterExport*>::iterator end = OSFloaterExport::instances.end();
		for( ; iter != end; ++iter)
		{
			(*iter)->receivePrimName(object, name, desc);
		}
	}
}