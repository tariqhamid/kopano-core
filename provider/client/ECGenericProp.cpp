/*
 * Copyright 2005 - 2016 Zarafa and its licensors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <kopano/platform.h>
#include <kopano/lockhelper.hpp>
#include <kopano/ECInterfaceDefs.h>
#include <mapidefs.h>
#include "WSTransport.h"
#include "ECGenericProp.h"

#include "kcore.hpp"
#include "pcutil.hpp"
#include "Mem.h"
#include <kopano/Util.h>

#include <kopano/ECGuid.h>

#include <kopano/ECDebug.h>

#include <kopano/charset/convert.h>
#include "EntryPoint.h"

ECGenericProp::ECGenericProp(void *lpProvider, ULONG ulObjType, BOOL fModify,
    const char *szClassName) :
	ECUnknown(szClassName)
{
	this->lstProps		= NULL;
	this->lpStorage		= NULL;
	this->fSaved		= false; // not saved until we either read or write from/to disk
	this->ulObjType		= ulObjType;
	this->fModify		= fModify;
	this->dwLastError	= hrSuccess;
	this->lpProvider	= lpProvider;
	this->isTransactedObject = TRUE; // only ECMsgStore and ECMAPIFolder are not transacted
	this->ulObjFlags	= 0;
	this->m_sMapiObject = NULL;
	this->m_ulMaxPropSize = 8192;

	m_lpEntryId = NULL;
	m_cbEntryId = 0;
	m_bReload = FALSE;
	m_bLoading = FALSE;

	this->HrAddPropHandlers(PR_EC_OBJECT,				DefaultGetProp,			DefaultSetPropComputed, (void*) this, FALSE, TRUE);
	this->HrAddPropHandlers(PR_NULL,					DefaultGetProp,			DefaultSetPropIgnore,	(void*) this, FALSE, TRUE);
	this->HrAddPropHandlers(PR_OBJECT_TYPE,				DefaultGetProp,			DefaultSetPropComputed, (void*) this);
	this->HrAddPropHandlers(PR_ENTRYID,					DefaultGetProp,			DefaultSetPropComputed, (void*) this);
}

ECGenericProp::~ECGenericProp()
{
	ECPropertyEntryIterator iterProps;

	if (m_sMapiObject)
		FreeMapiObject(m_sMapiObject);

	if(lstProps) {
		for (auto &i : *lstProps)
			i.second.DeleteProperty();
		delete lstProps;
	}

	if(lpStorage)
		lpStorage->Release();
	MAPIFreeBuffer(m_lpEntryId);
}

HRESULT ECGenericProp::QueryInterface(REFIID refiid, void **lppInterface)
{
	REGISTER_INTERFACE(IID_ECUnknown, this);

	return MAPI_E_INTERFACE_NOT_SUPPORTED;
}

HRESULT ECGenericProp::SetProvider(void* lpProvider)
{
	HRESULT hr = hrSuccess;
	assert(this->lpProvider == NULL);
	this->lpProvider = lpProvider;
	
	return hr;
}

HRESULT ECGenericProp::SetEntryId(ULONG cbEntryId, LPENTRYID lpEntryId)
{
	assert(m_lpEntryId == NULL);
	return Util::HrCopyEntryId(cbEntryId, lpEntryId, &m_cbEntryId, &m_lpEntryId);
}

// Add a property handler. Usually called by a subclass
HRESULT ECGenericProp::HrAddPropHandlers(ULONG ulPropTag, GetPropCallBack lpfnGetProp, SetPropCallBack lpfnSetProp, void *lpParam, BOOL fRemovable, BOOL fHidden)
{
	HRESULT					hr = hrSuccess;
	ECPropCallBackIterator	iterCallBack;
	PROPCALLBACK			sCallBack;

	// Check if the handler defines the right type, If Unicode you should never define a PT_STRING8 as handler!
	assert(PROP_TYPE(ulPropTag) == PT_STRING8 ||
	       PROP_TYPE(ulPropTag) == PT_UNICODE ?
	       PROP_TYPE(ulPropTag) == PT_TSTRING : true);

	// Only Support properties on ID, different types are not supported.
	iterCallBack = lstCallBack.find(PROP_ID(ulPropTag));
	if(iterCallBack != lstCallBack.end())
		lstCallBack.erase(iterCallBack);

	sCallBack.lpfnGetProp = lpfnGetProp;
	sCallBack.lpfnSetProp = lpfnSetProp;
	sCallBack.ulPropTag = ulPropTag;
	sCallBack.lpParam = lpParam;
	sCallBack.fRemovable = fRemovable;
	sCallBack.fHidden = fHidden;

	lstCallBack.insert(std::make_pair(PROP_ID(ulPropTag), sCallBack));

	dwLastError = hr;
	return hr;
}

// sets an actual value in memory
HRESULT ECGenericProp::HrSetRealProp(SPropValue *lpsPropValue)
{
	HRESULT					hr = hrSuccess;
	ECProperty*				lpProperty = NULL;
	ECPropertyEntryIterator	iterProps;
	ECPropertyEntryIterator iterPropsFound;
	ULONG ulPropId = 0;
	
	//FIXME: check the property structure -> lpsPropValue

	if (m_bLoading == FALSE && m_sMapiObject) {
		// Only reset instance id when we're being modified, not being reloaded
		HrSIEntryIDToID(m_sMapiObject->cbInstanceID, (LPBYTE)m_sMapiObject->lpInstanceID, NULL, NULL, (unsigned int *)&ulPropId);
		if (ulPropId == PROP_ID(lpsPropValue->ulPropTag))
			SetSingleInstanceId(0, NULL);
	}

	if(lstProps == NULL) {
		hr = HrLoadProps();
		if(hr != hrSuccess)
			goto exit;
	}			

	iterPropsFound = lstProps->end();
	// Loop through all properties, get the first EXACT matching property, but delete ALL
	// other properties with this PROP_ID and the wrong type - this makes sure you can SetProps() with 0x60010003,
	// then with 0x60010102 and then with 0x60010040 for example.

	iterProps = lstProps->find(PROP_ID(lpsPropValue->ulPropTag));
	if (iterProps != lstProps->end()) {
		if (iterProps->second.GetPropTag() != lpsPropValue->ulPropTag) {
			// type is different, remove the property and insert a new item
			m_setDeletedProps.insert(lpsPropValue->ulPropTag);

			iterProps->second.DeleteProperty();

			lstProps->erase(iterProps);
		} else {
			iterPropsFound = iterProps;
		}
	}

	// Changing an existing property
	if(iterPropsFound != lstProps->end()) {
		iterPropsFound->second.HrSetProp(lpsPropValue);
	} else { // Add new property
		lpProperty = new ECProperty(lpsPropValue);

		if(lpProperty->GetLastError() != 0) {
			hr = lpProperty->GetLastError();
			goto exit;
		}

		ECPropertyEntry entry(lpProperty);

		lstProps->insert(std::make_pair(PROP_ID(lpsPropValue->ulPropTag), entry));
	}

	// Property is now added/modified and marked 'dirty' for saving
exit:
	if (hr != hrSuccess)
		delete lpProperty;

	dwLastError = hr;
	return hr;
}

// Get an actual value from the property array and saves it to the given address. Any extra memory required
// is allocated with MAPIAllocMore with lpBase as the base pointer
//
// Properties are always returned unless:
//
// 1) The underlying system didn't return them in the initial HrReadProps call, and as such are 'too large' and
//    the property has not been force-loaded with OpenProperty
// or
// 2) A MaxSize was specified and the property is larger than that size (normally 8k or so)

HRESULT ECGenericProp::HrGetRealProp(ULONG ulPropTag, ULONG ulFlags, void *lpBase, LPSPropValue lpsPropValue, ULONG ulMaxSize)
{
	HRESULT					hr = hrSuccess;
	ECPropertyEntryIterator iterProps;
	
	if(lstProps == NULL || m_bReload == TRUE) {
		hr = HrLoadProps();
		if(hr != hrSuccess)
			goto exit;
		m_bReload = FALSE;
	}			

	// Find the property in our list
	iterProps = lstProps->find(PROP_ID(ulPropTag));

	// Not found or property is not matching
	if(iterProps == lstProps->end() || !( PROP_TYPE(ulPropTag) == PT_UNSPECIFIED || PROP_TYPE(ulPropTag) == PROP_TYPE(iterProps->second.GetPropTag()) ||
		(((ulPropTag & MV_FLAG) == (iterProps->second.GetPropTag( ) & MV_FLAG)) && PROP_TYPE(ulPropTag&~MV_FLAG) == PT_STRING8 && PROP_TYPE(iterProps->second.GetPropTag()&~MV_FLAG) == PT_UNICODE) ))
	{
		lpsPropValue->ulPropTag = PROP_TAG(PT_ERROR,PROP_ID(ulPropTag));
		lpsPropValue->Value.err = MAPI_E_NOT_FOUND;
		hr = MAPI_W_ERRORS_RETURNED;
		goto exit;
	}

	if(!iterProps->second.FIsLoaded()) {
		lpsPropValue->ulPropTag = PROP_TAG(PT_ERROR, PROP_ID(ulPropTag));
		lpsPropValue->Value.err = MAPI_E_NOT_ENOUGH_MEMORY;
		hr = MAPI_W_ERRORS_RETURNED;
		goto exit;

		// The load should have loaded into the value pointed to by iterProps, so we can use that now
	}

	// Check if a max. size was requested, if so, dont return unless smaller than max. size
	if (ulMaxSize != 0 && iterProps->second.GetProperty()->GetSize() > ulMaxSize) {
		lpsPropValue->ulPropTag = PROP_TAG(PT_ERROR, PROP_ID(ulPropTag));
		lpsPropValue->Value.err = MAPI_E_NOT_ENOUGH_MEMORY;
		hr = MAPI_W_ERRORS_RETURNED;
		goto exit;
	}

	if (PROP_TYPE(ulPropTag) == PT_UNSPECIFIED) {
		if (PROP_TYPE(iterProps->second.GetPropTag()) == PT_UNICODE)
			ulPropTag = CHANGE_PROP_TYPE(ulPropTag, ((ulFlags & MAPI_UNICODE) ? PT_UNICODE : PT_STRING8));
		else if (PROP_TYPE(iterProps->second.GetPropTag()) == PT_MV_UNICODE)
			ulPropTag = CHANGE_PROP_TYPE(ulPropTag, ((ulFlags & MAPI_UNICODE) ? PT_MV_UNICODE : PT_MV_STRING8));
		else
			ulPropTag = iterProps->second.GetPropTag();
	}

	// Copy the property to its final destination, with base pointer for extra allocations, if required.
	iterProps->second.GetProperty()->CopyTo(lpsPropValue, lpBase, ulPropTag);

exit:
	dwLastError = hr;

	return hr;
}

/** 
 * Deletes a property from the internal system
 * 
 * @param ulPropTag The requested ulPropTag to remove. Property type is ignored; only the property identifier is used.
 * @param fOverwriteRO Set to TRUE if the object is to be modified eventhough it's read-only. Currently unused! @todo parameter should be removed.
 * 
 * @return MAPI error code
 * @retval hrSuccess PropTag is set to be removed during the next SaveChanges call
 * @retval MAPI_E_NOT_FOUND The PropTag is not found in the list (can be type mismatch too)
 */
HRESULT ECGenericProp::HrDeleteRealProp(ULONG ulPropTag, BOOL fOverwriteRO)
{
	HRESULT					hr = hrSuccess;
	ECPropertyEntryIterator iterProps;

	if(lstProps == NULL) {
		hr = HrLoadProps();
		if(hr != hrSuccess)
			goto exit;
	}			

	// Now find the real value
	iterProps = lstProps->find(PROP_ID(ulPropTag));
	if(iterProps == lstProps->end()) {
		// Couldn't find it!
		hr = MAPI_E_NOT_FOUND;
		goto exit;
	}

	m_setDeletedProps.insert(iterProps->second.GetPropTag());

	iterProps->second.DeleteProperty();

	lstProps->erase(iterProps);

exit:
	dwLastError = hr;
	return hr;
}

// Default property handles
HRESULT	ECGenericProp::DefaultGetProp(ULONG ulPropTag,  void* lpProvider, ULONG ulFlags, LPSPropValue lpsPropValue, void *lpParam, void *lpBase)
{
	HRESULT			hr = hrSuccess;
	ECGenericProp*	lpProp = (ECGenericProp *)lpParam;

	switch(PROP_ID(ulPropTag))
	{
		case PROP_ID(PR_ENTRYID):
			if(lpProp->m_cbEntryId) {
				lpsPropValue->ulPropTag = PR_ENTRYID;
				lpsPropValue->Value.bin.cb = lpProp->m_cbEntryId;
				if(lpBase == NULL)
					assert(false);

				ECAllocateMore(lpProp->m_cbEntryId, lpBase, (void **)&lpsPropValue->Value.bin.lpb);
				memcpy(lpsPropValue->Value.bin.lpb, lpProp->m_lpEntryId, lpProp->m_cbEntryId);
			} else {
				hr = MAPI_E_NOT_FOUND;
			}
			break;

		// Gives access to the actual ECUnknown underlying object
		case PROP_ID(PR_EC_OBJECT):
			// NOTE: we place the object pointer in lpszA to make sure it's on the same offset as Value.x on 32bit as 64bit machines
			lpsPropValue->ulPropTag = PR_EC_OBJECT;
			lpsPropValue->Value.lpszA = (LPSTR)lpProp;
			break;

		case PROP_ID(PR_NULL):
			// outlook with export contacts to csv (IMessage)(0x00000000) <- skip this one
			// Palm used PR_NULL (IMAPIFolder)(0x00000001)
			if(ulPropTag == PR_NULL) {
				lpsPropValue->ulPropTag = PR_NULL;
				memset(&lpsPropValue->Value, 0, sizeof(lpsPropValue->Value)); // make sure all bits, 32 or 64, are 0
			} else {
				hr = MAPI_E_NOT_FOUND;
			}
			break;

		case PROP_ID(PR_OBJECT_TYPE): 
			lpsPropValue->Value.l = lpProp->ulObjType;
			lpsPropValue->ulPropTag = PR_OBJECT_TYPE;
			break;

		default:
			hr = lpProp->HrGetRealProp(ulPropTag, ulFlags, lpBase, lpsPropValue);
			break;
	}

	return hr;
}

HRESULT	ECGenericProp::DefaultGetPropGetReal(ULONG ulPropTag, void* lpProvider, ULONG ulFlags, LPSPropValue lpsPropValue, void *lpParam, void *lpBase)
{
	ECGenericProp *lpProp = (ECGenericProp *)lpParam;

	return lpProp->HrGetRealProp(ulPropTag, ulFlags, lpBase, lpsPropValue, lpProp->m_ulMaxPropSize);
}

HRESULT	ECGenericProp::DefaultGetPropNotFound(ULONG ulPropTag, void* lpProvider, ULONG ulFlags, LPSPropValue lpsPropValue, void *lpParam, void *lpBase)
{
	return MAPI_E_NOT_FOUND;
}

HRESULT ECGenericProp::DefaultSetPropSetReal(ULONG ulPropTag, void* lpProvider, LPSPropValue lpsPropValue, void *lpParam)
{
	ECGenericProp *lpProp = (ECGenericProp *)lpParam;

	return lpProp->HrSetRealProp(lpsPropValue);
}

HRESULT	ECGenericProp::DefaultSetPropComputed(ULONG ulPropTag, void* lpProvider, LPSPropValue lpsPropValue, void *lpParam)
{
	return MAPI_E_COMPUTED;
}

HRESULT	ECGenericProp::DefaultSetPropIgnore(ULONG ulPropTag, void* lpProvider, LPSPropValue lpsPropValue, void *lpParam)
{
	return hrSuccess;
}

HRESULT ECGenericProp::TableRowGetProp(void* lpProvider, struct propVal *lpsPropValSrc, LPSPropValue lpsPropValDst, void **lpBase, ULONG ulType)
{
	HRESULT hr = hrSuccess;

	switch(lpsPropValSrc->ulPropTag) {
		case PROP_TAG(PT_ERROR,PROP_ID(PR_NULL)): 
			lpsPropValDst->Value.l = 0;
			lpsPropValDst->ulPropTag = PR_NULL;
			break;
		default:
			hr = MAPI_E_NOT_FOUND;
			break;
	}

	return hr;
}

// Sets all the properties 'clean', ie. un-dirty
HRESULT ECGenericProp::HrSetClean()
{
	HRESULT hr = hrSuccess;
	ECPropertyEntryIterator iterProps;

	// also remove deleted marked properties, since the object isn't reloaded from the server anymore
	for (iterProps = lstProps->begin(); iterProps != lstProps->end(); ++iterProps)
		iterProps->second.HrSetClean();

	m_setDeletedProps.clear();

	return hr;
}

HRESULT ECGenericProp::HrSaveChild(ULONG ulFlags, MAPIOBJECT *lpsMapiObject)
{
	// ECMessage implements saving an attachment
	// ECAttach implements saving a sub-message
	return MAPI_E_INVALID_OBJECT;
}

HRESULT ECGenericProp::HrRemoveModifications(MAPIOBJECT *lpsMapiObject, ULONG ulPropTag)
{
	HRESULT hr = hrSuccess;

	lpsMapiObject->lstDeleted->remove(ulPropTag);
	for (auto iterProps = lpsMapiObject->lstModified->begin();
	     iterProps != lpsMapiObject->lstModified->end(); ++iterProps)
		if(iterProps->GetPropTag() == ulPropTag) {
			lpsMapiObject->lstModified->erase(iterProps);
			break;
		}
	return hr;
}

HRESULT ECGenericProp::GetLastError(HRESULT hResult, ULONG ulFlags, LPMAPIERROR *lppMAPIError)
{
	HRESULT		hr = hrSuccess;
	LPMAPIERROR	lpMapiError = NULL;
	LPTSTR		lpszErrorMsg = NULL;
	
	hr = Util::HrMAPIErrorToText((hResult == hrSuccess)?MAPI_E_NO_ACCESS : hResult, &lpszErrorMsg);
	if (hr != hrSuccess)
		goto exit;

	hr = ECAllocateBuffer(sizeof(MAPIERROR),(void **)&lpMapiError);
	if(hr != hrSuccess)
		goto exit;
		
	if ((ulFlags & MAPI_UNICODE) == MAPI_UNICODE) {
		std::wstring wstrErrorMsg = convert_to<std::wstring>(lpszErrorMsg);
		std::wstring wstrCompName = convert_to<std::wstring>(g_strProductName.c_str());

		if ((hr = MAPIAllocateMore(sizeof(std::wstring::value_type) * (wstrErrorMsg.size() + 1), lpMapiError, (void**)&lpMapiError->lpszError)) != hrSuccess)
			goto exit;
		wcscpy((wchar_t*)lpMapiError->lpszError, wstrErrorMsg.c_str());

		if ((hr = MAPIAllocateMore(sizeof(std::wstring::value_type) * (wstrCompName.size() + 1), lpMapiError, (void**)&lpMapiError->lpszComponent)) != hrSuccess)
			goto exit;
		wcscpy((wchar_t*)lpMapiError->lpszComponent, wstrCompName.c_str()); 

	} else {
		std::string strErrorMsg = convert_to<std::string>(lpszErrorMsg);
		std::string strCompName = convert_to<std::string>(g_strProductName.c_str());

		if ((hr = MAPIAllocateMore(strErrorMsg.size() + 1, lpMapiError, (void**)&lpMapiError->lpszError)) != hrSuccess)
			goto exit;
		strcpy((char*)lpMapiError->lpszError, strErrorMsg.c_str());

		if ((hr = MAPIAllocateMore(strCompName.size() + 1, lpMapiError, (void**)&lpMapiError->lpszComponent)) != hrSuccess)
			goto exit;
		strcpy((char*)lpMapiError->lpszComponent, strCompName.c_str());
	}

	lpMapiError->ulContext		= 0;
	lpMapiError->ulLowLevelError= 0;
	lpMapiError->ulVersion		= 0;

	*lppMAPIError = lpMapiError;

exit:
	MAPIFreeBuffer(lpszErrorMsg);
	if( hr != hrSuccess && lpMapiError)
		ECFreeBuffer(lpMapiError);

	return hr;
}

// Differential save of changed properties
HRESULT ECGenericProp::SaveChanges(ULONG ulFlags)
{
	HRESULT			hr = hrSuccess;
	scoped_rlock l_obj(m_hMutexMAPIObject);

	if (!fModify) {
		hr = MAPI_E_NO_ACCESS;
		goto exit;
	}

	if (!m_sMapiObject || !lstProps) {
		hr = MAPI_E_CALL_FAILED;
		goto exit;
	}

	// no props -> succeed (no changes made)
	if(lstProps->empty())
		goto exit;

	if(lpStorage == NULL) {
		// no way to save our properties !
		hr = MAPI_E_NO_ACCESS;
		goto exit;
	}

	// Note: m_sMapiObject->lstProperties and m_sMapiObject->lstAvailable are empty
	// here, because they are cleared after HrLoadProps and SaveChanges

	// save into m_sMapiObject
	
	for (auto l : m_setDeletedProps) {
		// Make sure the property is not present in deleted/modified list
		HrRemoveModifications(m_sMapiObject, l);
		m_sMapiObject->lstDeleted->push_back(l);
	}

	for (auto &p : *lstProps) {
		// Property is dirty, so we have to save it
		if (p.second.FIsDirty()) {
			// Save in the 'modified' list

			// Make sure the property is not present in deleted/modified list
			HrRemoveModifications(m_sMapiObject, p.second.GetPropTag());
			// Save modified property
			m_sMapiObject->lstModified->push_back(*p.second.GetProperty());
			// Save in the normal properties list
			m_sMapiObject->lstProperties->push_back(*p.second.GetProperty());
			continue;
		}

		// Normal property: either non-loaded or loaded
		if (!p.second.FIsLoaded())	// skip pt_error anyway
			m_sMapiObject->lstAvailable->push_back(p.second.GetPropTag());
		else
			m_sMapiObject->lstProperties->push_back(*p.second.GetProperty());
	}

	m_sMapiObject->bChanged = true;

	// Our s_MapiObject now contains its full property list in lstProperties and lstAvailable,
	// and its modifications in lstModified and lstDeleted.

	// save to parent or server
	hr = lpStorage->HrSaveObject(this->ulObjFlags, m_sMapiObject);
	if (hr != hrSuccess)
		goto exit;

	// HrSaveObject() has appended any new properties in lstAvailable and lstProperties. We need to load the 
	// new properties. The easiest way to do this is to simply load all properties. Note that in embedded objects
	// that save to ECParentStorage, the object will be untouched. The code below will do nothing.

	// Large properties received
	for (auto tag : *m_sMapiObject->lstAvailable) {
		// ONLY if not present
		auto ip = lstProps->find(PROP_ID(tag));
		if (ip == lstProps->cend() || ip->second.GetPropTag() != tag) {
			ECPropertyEntry entry(tag);
			lstProps->insert(std::make_pair(PROP_ID(tag), entry));
		}
	}
	m_sMapiObject->lstAvailable->clear();

	// Normal properties with value
	for (const auto &pv : *m_sMapiObject->lstProperties)
		// don't add any 'error' types ... (the storage object shouldn't really give us these anyway ..)
		if (PROP_TYPE(pv.GetPropTag()) != PT_ERROR) {
			SPropValue tmp = pv.GetMAPIPropValRef();
			HrSetRealProp(&tmp);
		}

	// Note that we currently don't support the server removing properties after the SaveObject call

	// We have loaded all properties, so clear the properties in the m_sMapiObject
	m_sMapiObject->lstProperties->clear();
	m_sMapiObject->lstAvailable->clear();

	// We are now in sync with the server again, so set everything as clean
	HrSetClean();

	fSaved = true;

exit:
	if (hr == hrSuccess)
		// Unless the user requests to continue with modify access, switch
		// down to read-only access. This means that specifying neither of
		// the KEEP_OPEN flags means the same thing as KEEP_OPEN_READONLY.
		if (!(ulFlags & (KEEP_OPEN_READWRITE|FORCE_SAVE)))
			fModify = FALSE;
	return hr;
}

// Check if property is dirty (delete properties gives MAPI_E_NOT_FOUND)
HRESULT ECGenericProp::IsPropDirty(ULONG ulPropTag, BOOL *lpbDirty)
{
	ECPropertyEntryIterator iterProps;

	iterProps = lstProps->find(PROP_ID(ulPropTag));
	if (iterProps == lstProps->end() || (PROP_TYPE(ulPropTag) != PT_UNSPECIFIED && ulPropTag != iterProps->second.GetPropTag()))
		return MAPI_E_NOT_FOUND;
	
	*lpbDirty = iterProps->second.FIsDirty();
	return hrSuccess;
}

/**
 * Clears the 'dirty' flag for a property
 *
 * This means that during a save the property will not be sent to the server. The dirty flag will be set
 * again after a HrSetRealProp() again.
 *
 * @param[in] ulPropTag Property to mark un-dirty
 * @result HRESULT
 */
HRESULT ECGenericProp::HrSetCleanProperty(ULONG ulPropTag)
{
	ECPropertyEntryIterator iterProps;

	iterProps = lstProps->find(PROP_ID(ulPropTag));
	if (iterProps == lstProps->end() || (PROP_TYPE(ulPropTag) != PT_UNSPECIFIED && ulPropTag != iterProps->second.GetPropTag()))
		return MAPI_E_NOT_FOUND;
	
	iterProps->second.HrSetClean();
	return hrSuccess;
}

// Get the handler(s) for a given property tag
HRESULT	ECGenericProp::HrGetHandler(ULONG ulPropTag, SetPropCallBack *lpfnSetProp, GetPropCallBack *lpfnGetProp, void **lpParam)
{
	HRESULT					hr = hrSuccess;
	ECPropCallBackIterator iterCallBack;

	iterCallBack = lstCallBack.find(PROP_ID(ulPropTag));
	if(iterCallBack == lstCallBack.end() || 
		(ulPropTag != iterCallBack->second.ulPropTag && PROP_TYPE(ulPropTag) != PT_UNSPECIFIED &&
		!(PROP_TYPE(iterCallBack->second.ulPropTag) == PT_TSTRING && (PROP_TYPE(ulPropTag) == PT_STRING8 || PROP_TYPE(ulPropTag) == PT_UNICODE) )
		) ) {
		hr = MAPI_E_NOT_FOUND;
		goto exit;
	}

	if(lpfnSetProp)
		*lpfnSetProp = iterCallBack->second.lpfnSetProp;

	if(lpfnGetProp)
		*lpfnGetProp = iterCallBack->second.lpfnGetProp;

	if(lpParam)
		*lpParam = iterCallBack->second.lpParam;
	
exit:
	dwLastError = hr;
	return hr;
}

HRESULT ECGenericProp::HrSetPropStorage(IECPropStorage *lpStorage, BOOL fLoadProps)
{
	HRESULT hr;
	SPropValue sPropValue;

	if(this->lpStorage)
		this->lpStorage->Release();

	this->lpStorage = lpStorage;

	if(lpStorage)
		lpStorage->AddRef();

	if(fLoadProps) {
		hr = HrLoadProps();
		if(hr != hrSuccess)
			return hr;
		if (HrGetRealProp(PR_OBJECT_TYPE, 0, NULL, &sPropValue, m_ulMaxPropSize) == hrSuccess &&
		    // The server sent a PR_OBJECT_TYPE, check if it is correct
		    this->ulObjType != sPropValue.Value.ul)
			// Return NOT FOUND because the entryid given was the incorrect type. This means
			// that the object was basically not found.
			return MAPI_E_NOT_FOUND;
	}
	return hrSuccess;
}

HRESULT ECGenericProp::HrLoadEmptyProps()
{
	scoped_rlock lock(m_hMutexMAPIObject);

	assert(lstProps == NULL);
	assert(m_sMapiObject == NULL);
	lstProps = new ECPropertyEntryMap;
	AllocNewMapiObject(0, 0, ulObjType, &m_sMapiObject);
	return hrSuccess;
}

// Loads the properties of the saved message for use
HRESULT ECGenericProp::HrLoadProps()
{
	HRESULT			hr = hrSuccess;

	if(lpStorage == NULL)
		return MAPI_E_CALL_FAILED;

	scoped_rlock lock(m_hMutexMAPIObject);

	if (lstProps != NULL && m_bReload == FALSE)
		goto exit; // already loaded

	m_bLoading = TRUE;

	if (m_sMapiObject != NULL) {
		// remove what we know, (scenario: keep open r/w, drop props, get all again causes to know the server changes, incl. the hierarchy id)
		FreeMapiObject(m_sMapiObject);
		m_sMapiObject = NULL;

		// only remove my own properties: keep recipients and attachment tables
		if (lstProps != NULL) {
			for (auto &p : *lstProps)
				p.second.DeleteProperty();
			lstProps->clear();
		}
		m_setDeletedProps.clear();
	}

	hr = lpStorage->HrLoadObject(&m_sMapiObject);
	if (hr != hrSuccess)
		goto exit;

	if (lstProps == NULL)
		lstProps = new ECPropertyEntryMap;

	// Add *all* the entries as with empty values; values for these properties will be
	// retrieved on-demand
	for (auto tag : *m_sMapiObject->lstAvailable) {
		ECPropertyEntry entry(tag);
		lstProps->insert(std::make_pair(PROP_ID(tag), entry));
	}

	// Load properties
	for (const auto &pv : *m_sMapiObject->lstProperties)
		// don't add any 'error' types ... (the storage object shouldn't really give us these anyway ..)
		if (PROP_TYPE(pv.GetPropTag()) != PT_ERROR) {
			SPropValue tmp = pv.GetMAPIPropValRef();
			HrSetRealProp(&tmp);
		}

	// remove copied proptags, subobjects are still present
	m_sMapiObject->lstAvailable->clear();
	m_sMapiObject->lstProperties->clear(); // pointers are now only present in lstProps (this removes memory usage!)

	// at this point: children still known, ulObjId and ulObjType too

	// Mark all properties now in memory as 'clean' (need not be saved)
	hr = HrSetClean();

	if(hr != hrSuccess)
		goto exit;

	// We just read the properties from the disk, so it is a 'saved' (ie on-disk) message
	fSaved = true;

exit:
	dwLastError = hr;
	m_bReload = FALSE;
	m_bLoading = FALSE;
	return hr;
}

// Load a single (large) property from the storage
HRESULT ECGenericProp::HrLoadProp(ULONG ulPropTag)
{
	HRESULT			hr = hrSuccess;
	LPSPropValue	lpsPropVal = NULL;

	ECPropertyEntryIterator	iterProps;

	if(lpStorage == NULL)
		return MAPI_E_CALL_FAILED;

	ulPropTag = NormalizePropTag(ulPropTag);

	scoped_rlock lock(m_hMutexMAPIObject);

	if(lstProps == NULL || m_bReload == TRUE) {
		hr = HrLoadProps();
		if(hr != hrSuccess)
			goto exit;
	}			

	iterProps = lstProps->find(PROP_ID(ulPropTag));
	if(iterProps == lstProps->end() || (PROP_TYPE(ulPropTag) != PT_UNSPECIFIED && PROP_TYPE(ulPropTag) != PROP_TYPE(iterProps->second.GetPropTag())) ) {
		hr = MAPI_E_NOT_FOUND;
		goto exit;
	}

	// Don't load the data if it was already loaded
	if(iterProps->second.FIsLoaded()) {
		hr = MAPI_E_NOT_FOUND;
		goto exit;
	}

  	// The property was not loaded yet, demand-load it now
	hr = lpStorage->HrLoadProp(m_sMapiObject->ulObjId, iterProps->second.GetPropTag(), &lpsPropVal);
	if(hr != hrSuccess)
		goto exit;

	hr = iterProps->second.HrSetProp(new ECProperty(lpsPropVal));
	if(hr != hrSuccess)
		goto exit;

	// It's clean 'cause we just loaded it
	iterProps->second.HrSetClean();

exit:
	if(lpsPropVal)
		ECFreeBuffer(lpsPropVal);
	return hr;
}

HRESULT ECGenericProp::GetProps(LPSPropTagArray lpPropTagArray, ULONG ulFlags, ULONG *lpcValues, LPSPropValue *lppPropArray)
{
	HRESULT			hr = hrSuccess;
	HRESULT			hrT = hrSuccess;
	SPropTagArray *lpGetPropTagArray = NULL;
	GetPropCallBack	lpfnGetProp = NULL;
	void*			lpParam = NULL;
	LPSPropValue	lpsPropValue = NULL;
	unsigned int	i;

	//FIXME: check lpPropTagArray on PROP_TYPE()
	if((lpPropTagArray != NULL && lpPropTagArray->cValues == 0) || Util::ValidatePropTagArray(lpPropTagArray) == false)
		return MAPI_E_INVALID_PARAMETER;

	if (lpPropTagArray == NULL) {
		hr = GetPropList(ulFlags, &lpGetPropTagArray);

		if(hr != hrSuccess)
			goto exit;
		lpPropTagArray = lpGetPropTagArray;
	}

	ECAllocateBuffer(sizeof(SPropValue) * lpPropTagArray->cValues,
		reinterpret_cast<void **>(&lpsPropValue));

	for (i = 0; i < lpPropTagArray->cValues; ++i) {
		if (HrGetHandler(lpPropTagArray->aulPropTag[i], NULL, &lpfnGetProp, &lpParam) == hrSuccess) {
			lpsPropValue[i].ulPropTag = lpPropTagArray->aulPropTag[i];
			hrT = lpfnGetProp(lpPropTagArray->aulPropTag[i], this->lpProvider, ulFlags, &lpsPropValue[i], lpParam, lpsPropValue);
		} else {
			hrT = HrGetRealProp(lpPropTagArray->aulPropTag[i], ulFlags, lpsPropValue, &lpsPropValue[i], m_ulMaxPropSize);
			if(hrT != hrSuccess && hrT != MAPI_E_NOT_FOUND && hrT != MAPI_E_NOT_ENOUGH_MEMORY && hrT != MAPI_W_ERRORS_RETURNED) {
				hr = hrT;
				goto exit;
			}
		}

		if(HR_FAILED(hrT)) {
			lpsPropValue[i].ulPropTag = PROP_TAG(PT_ERROR, PROP_ID(lpPropTagArray->aulPropTag[i]));
			lpsPropValue[i].Value.err = hrT;
			hr = MAPI_W_ERRORS_RETURNED;
		} else if(hrT != hrSuccess) {
			hr = MAPI_W_ERRORS_RETURNED;
		}
	}

	*lppPropArray = lpsPropValue;
	*lpcValues = lpPropTagArray->cValues;
exit:
	if (lpGetPropTagArray != NULL)
		ECFreeBuffer(lpGetPropTagArray);

	return hr;

}

HRESULT ECGenericProp::GetPropList(ULONG ulFlags, LPSPropTagArray *lppPropTagArray)
{
	HRESULT hr;
	LPSPropTagArray		lpPropTagArray = NULL;
	int					n = 0;

	ECPropCallBackIterator	iterCallBack;
	ECPropertyEntryIterator	iterProps;
	
	if(lstProps == NULL) {
		hr = HrLoadProps();
		if(hr != hrSuccess)
			return hr;
	}			

	// The size of the property tag array is never larger than (static properties + generated properties)
	ECAllocateBuffer(CbNewSPropTagArray(lstProps->size() + lstCallBack.size()), (LPVOID *)&lpPropTagArray);

	// Some will overlap so we've actually allocated slightly too much memory

	// Add the callback types first
	for (iterCallBack = lstCallBack.begin();
	     iterCallBack != lstCallBack.end(); ++iterCallBack) {
		// Don't add 'hidden' properties
		if(iterCallBack->second.fHidden)
			continue;

		// Check if the callback actually returns OK
		// a bit wasteful but fine for now.

		LPSPropValue lpsPropValue = NULL;
		HRESULT hrT = hrSuccess;

		ECAllocateBuffer(sizeof(SPropValue), (LPVOID *)&lpsPropValue);

		hrT = iterCallBack->second.lpfnGetProp(iterCallBack->second.ulPropTag, this->lpProvider, ulFlags, lpsPropValue, this, lpsPropValue);

		if((!HR_FAILED(hrT) || hrT == MAPI_E_NOT_ENOUGH_MEMORY) && (PROP_TYPE(lpsPropValue->ulPropTag) != PT_ERROR || lpsPropValue->Value.err == MAPI_E_NOT_ENOUGH_MEMORY)) {
			ULONG ulPropTag = iterCallBack->second.ulPropTag;
			
			if (PROP_TYPE(ulPropTag) == PT_UNICODE ||
			    PROP_TYPE(ulPropTag) == PT_STRING8)
				ulPropTag = CHANGE_PROP_TYPE(ulPropTag, ((ulFlags & MAPI_UNICODE) ? PT_UNICODE : PT_STRING8));
			
			lpPropTagArray->aulPropTag[n++] = ulPropTag;
		}

		if(lpsPropValue)
			ECFreeBuffer(lpsPropValue);

	}

	// Then add the others, if not added yet
	for (iterProps = lstProps->begin(); iterProps != lstProps->end(); ++iterProps) {
		if(HrGetHandler(iterProps->second.GetPropTag(),NULL,NULL,NULL) != 0) {
			ULONG ulPropTag = iterProps->second.GetPropTag();

			if(!(ulFlags & MAPI_UNICODE)) {
				// Downgrade to ansi
				if(PROP_TYPE(ulPropTag) == PT_UNICODE)
					ulPropTag = PROP_TAG(PT_STRING8, PROP_ID(ulPropTag));
				else if(PROP_TYPE(ulPropTag) == PT_MV_UNICODE)
					ulPropTag = PROP_TAG(PT_MV_STRING8, PROP_ID(ulPropTag));
			}

			lpPropTagArray->aulPropTag[n++] = ulPropTag;
		}
	}

	lpPropTagArray->cValues = n;

	*lppPropTagArray = lpPropTagArray;
	return hrSuccess;
}

HRESULT ECGenericProp::OpenProperty(ULONG ulPropTag, LPCIID lpiid, ULONG ulInterfaceOptions, ULONG ulFlags, LPUNKNOWN *lppUnk)
{
	return  MAPI_E_NO_SUPPORT;
}

HRESULT ECGenericProp::SetProps(ULONG cValues, LPSPropValue lpPropArray, LPSPropProblemArray *lppProblems)
{
	HRESULT				hr = hrSuccess;
	HRESULT				hrT = hrSuccess;
	LPSPropProblemArray	lpProblems = NULL;
	int					nProblem = 0;
	SetPropCallBack		lpfnSetProp = NULL;
	void*				lpParam = NULL;

	if (lpPropArray == NULL) {
		hr = MAPI_E_INVALID_PARAMETER;
		goto exit;
	}
	
	hr = ECAllocateBuffer(CbNewSPropProblemArray(cValues), (LPVOID *)&lpProblems);
	if(hr != hrSuccess)
		goto exit;

	for (unsigned int i = 0; i < cValues; ++i) {
		// Ignore the PR_NULL property tag and all properties with a type of PT_ERROR;
		// no changes and report no problems in the SPropProblemArray structure. 
		if(PROP_TYPE(lpPropArray[i].ulPropTag) == PR_NULL ||
			PROP_TYPE(lpPropArray[i].ulPropTag) == PT_ERROR)
			continue;

		if (HrGetHandler(lpPropArray[i].ulPropTag, &lpfnSetProp, NULL, &lpParam) == hrSuccess)
			hrT = lpfnSetProp(lpPropArray[i].ulPropTag, this->lpProvider, &lpPropArray[i], lpParam);
		else
			hrT = HrSetRealProp(&lpPropArray[i]); // SC: TODO: this does a ref copy ?!

		if(hrT != hrSuccess) {
			lpProblems->aProblem[nProblem].scode = hrT;
			lpProblems->aProblem[nProblem].ulIndex = i;
			lpProblems->aProblem[nProblem].ulPropTag = lpPropArray[i].ulPropTag; // Hold here the real property
			++nProblem;
		}
	}

	lpProblems->cProblem = nProblem;

	if(lppProblems && nProblem) {
		*lppProblems = lpProblems;
		lpProblems = NULL; // Don't delete lpProblems
	} else if(lppProblems) {
		*lppProblems = NULL;
	}

exit:
	if(lpProblems)
		ECFreeBuffer(lpProblems);

	return hr;
}

/** 
 * Delete properties from the current object
 * 
 * @param lpPropTagArray PropTagArray with properties to remove from the object. If a property from this list is not found, it will be added to the lppProblems array.
 * @param lppProblems An array of proptags that could not be removed from the object. Returns NULL if everything requested was removed. Can be NULL not to want the problem array.
 * 
 * @remark Property types are ignored; only the property identifiers are used.
 *
 * @return MAPI error code
 * @retval hrSuccess 0 or more properties are set to be removed on SaveChanges
 * @retval MAPI_E_NO_ACCESS the object is read-only
 * @retval MAPI_E_NOT_ENOUGH_MEMORY unable to allocate memory to remove the problem array
 */
HRESULT ECGenericProp::DeleteProps(LPSPropTagArray lpPropTagArray, LPSPropProblemArray *lppProblems)
{
	ECRESULT				er = erSuccess;
	HRESULT					hr = hrSuccess;
	HRESULT					hrT = hrSuccess;
	ECPropCallBackIterator	iterCallBack;
	LPSPropProblemArray		lpProblems = NULL;
	int						nProblem = 0;

	if (lpPropTagArray == NULL)
		return MAPI_E_INVALID_PARAMETER;

	// over-allocate the problem array
	er = ECAllocateBuffer(CbNewSPropProblemArray(lpPropTagArray->cValues), (LPVOID *)&lpProblems);
	if (er != erSuccess)
		return MAPI_E_NOT_ENOUGH_MEMORY;

	for (unsigned int i = 0; i < lpPropTagArray->cValues; ++i) {
		// See if it's computed
		iterCallBack = lstCallBack.find(PROP_ID(lpPropTagArray->aulPropTag[i]));

		// Ignore removable callbacks
		if(iterCallBack != lstCallBack.end() && !iterCallBack->second.fRemovable) {
			// This is a computed value
			lpProblems->aProblem[nProblem].scode = MAPI_E_COMPUTED;
			lpProblems->aProblem[nProblem].ulIndex = i;
			lpProblems->aProblem[nProblem].ulPropTag = lpPropTagArray->aulPropTag[i];
			++nProblem;
		} else {

			hrT = HrDeleteRealProp(lpPropTagArray->aulPropTag[i],FALSE);

			if(hrT != hrSuccess) {
				// Add the error
				lpProblems->aProblem[nProblem].scode = hrT;
				lpProblems->aProblem[nProblem].ulIndex = i;
				lpProblems->aProblem[nProblem].ulPropTag = lpPropTagArray->aulPropTag[i];
				++nProblem;
			}
		}
	}

	lpProblems->cProblem = nProblem;

	if(lppProblems && nProblem)
		*lppProblems = lpProblems;
	else if(lppProblems) {
		*lppProblems = NULL;
		ECFreeBuffer(lpProblems);
	} else {
		ECFreeBuffer(lpProblems);
	}
	return hr;
}

HRESULT ECGenericProp::CopyTo(ULONG ciidExclude, LPCIID rgiidExclude, LPSPropTagArray lpExcludeProps, ULONG ulUIParam, LPMAPIPROGRESS lpProgress, LPCIID lpInterface, LPVOID lpDestObj, ULONG ulFlags, LPSPropProblemArray *lppProblems)
{
	return MAPI_E_NO_SUPPORT;
}

HRESULT ECGenericProp::CopyProps(LPSPropTagArray lpIncludeProps, ULONG ulUIParam, LPMAPIPROGRESS lpProgress, LPCIID lpInterface, LPVOID lpDestObj, ULONG ulFlags, LPSPropProblemArray *lppProblems)
{
	return MAPI_E_NO_SUPPORT;
}

HRESULT ECGenericProp::GetNamesFromIDs(LPSPropTagArray *lppPropTags, LPGUID lpPropSetGuid, ULONG ulFlags, ULONG *lpcPropNames, LPMAPINAMEID **lpppPropNames)
{
	return MAPI_E_NO_SUPPORT;
}
 
HRESULT ECGenericProp::GetIDsFromNames(ULONG cPropNames, LPMAPINAMEID *lppPropNames, ULONG ulFlags, LPSPropTagArray *lppPropTags)
{
	return MAPI_E_NO_SUPPORT;
}

// Interface IECSingleInstance
HRESULT ECGenericProp::GetSingleInstanceId(ULONG *lpcbInstanceID, LPSIEID *lppInstanceID)
{
	scoped_rlock lock(m_hMutexMAPIObject);

	if (m_sMapiObject == NULL)
		return MAPI_E_NOT_FOUND;
	if (lpcbInstanceID == NULL || lppInstanceID == NULL)
		return MAPI_E_INVALID_PARAMETER;
	return Util::HrCopyEntryId(m_sMapiObject->cbInstanceID,
	       reinterpret_cast<ENTRYID *>(m_sMapiObject->lpInstanceID),
	       lpcbInstanceID, reinterpret_cast<ENTRYID **>(lppInstanceID));
}

HRESULT ECGenericProp::SetSingleInstanceId(ULONG cbInstanceID, LPSIEID lpInstanceID)
{
	scoped_rlock lock(m_hMutexMAPIObject);

	if (m_sMapiObject == NULL)
		return MAPI_E_NOT_FOUND;
	if (m_sMapiObject->lpInstanceID)
		ECFreeBuffer(m_sMapiObject->lpInstanceID);

	m_sMapiObject->lpInstanceID = NULL;
	m_sMapiObject->cbInstanceID = 0;
	m_sMapiObject->bChangedInstance = false;

	HRESULT hr = Util::HrCopyEntryId(cbInstanceID,
		reinterpret_cast<ENTRYID *>(lpInstanceID),
		&m_sMapiObject->cbInstanceID,
		reinterpret_cast<ENTRYID **>(&m_sMapiObject->lpInstanceID));
	if (hr != hrSuccess)
		return hr;
	m_sMapiObject->bChangedInstance = true;
	return hr;
}

// Interface IMAPIProp
DEF_HRMETHOD0(ECGenericProp, MAPIProp, QueryInterface, (REFIID, refiid), (void **, lppInterface))
DEF_ULONGMETHOD0(ECGenericProp, MAPIProp, AddRef, (void))
DEF_ULONGMETHOD0(ECGenericProp, MAPIProp, Release, (void))
DEF_HRMETHOD0(ECGenericProp, MAPIProp, GetLastError, (HRESULT, hError), (ULONG, ulFlags), (LPMAPIERROR *, lppMapiError))
DEF_HRMETHOD0(ECGenericProp, MAPIProp, SaveChanges, (ULONG, ulFlags))
DEF_HRMETHOD0(ECGenericProp, MAPIProp, GetProps, (LPSPropTagArray, lpPropTagArray), (ULONG, ulFlags), (ULONG *, lpcValues), (LPSPropValue *, lppPropArray))
DEF_HRMETHOD0(ECGenericProp, MAPIProp, GetPropList, (ULONG, ulFlags), (LPSPropTagArray *, lppPropTagArray))
DEF_HRMETHOD0(ECGenericProp, MAPIProp, OpenProperty, (ULONG, ulPropTag), (LPCIID, lpiid), (ULONG, ulInterfaceOptions), (ULONG, ulFlags), (LPUNKNOWN *, lppUnk))
DEF_HRMETHOD0(ECGenericProp, MAPIProp, SetProps, (ULONG, cValues), (LPSPropValue, lpPropArray), (LPSPropProblemArray *, lppProblems))
DEF_HRMETHOD0(ECGenericProp, MAPIProp, DeleteProps, (LPSPropTagArray, lpPropTagArray), (LPSPropProblemArray *, lppProblems))
DEF_HRMETHOD0(ECGenericProp, MAPIProp, CopyTo, (ULONG, ciidExclude), (LPCIID, rgiidExclude), (LPSPropTagArray, lpExcludeProps), (ULONG, ulUIParam), (LPMAPIPROGRESS, lpProgress), (LPCIID, lpInterface), (LPVOID, lpDestObj), (ULONG, ulFlags), (LPSPropProblemArray *, lppProblems))
DEF_HRMETHOD0(ECGenericProp, MAPIProp, CopyProps, (LPSPropTagArray, lpIncludeProps), (ULONG, ulUIParam), (LPMAPIPROGRESS, lpProgress), (LPCIID, lpInterface), (LPVOID, lpDestObj), (ULONG, ulFlags), (LPSPropProblemArray *, lppProblems))
DEF_HRMETHOD0(ECGenericProp, MAPIProp, GetNamesFromIDs, (LPSPropTagArray *, pptaga), (LPGUID, lpguid), (ULONG, ulFlags), (ULONG *, pcNames), (LPMAPINAMEID **, pppNames))
DEF_HRMETHOD0(ECGenericProp, MAPIProp, GetIDsFromNames, (ULONG, cNames), (LPMAPINAMEID *, ppNames), (ULONG, ulFlags), (LPSPropTagArray *, pptaga))

// Proxy routines for IECSingleInstance
DEF_HRMETHOD1(TRACE_MAPI, ECGenericProp, ECSingleInstance, QueryInterface, (REFIID, refiid), (void **, lppInterface))
DEF_ULONGMETHOD1(TRACE_MAPI, ECGenericProp, ECSingleInstance, AddRef, (void))
DEF_ULONGMETHOD1(TRACE_MAPI, ECGenericProp, ECSingleInstance, Release, (void))

HRESULT __stdcall ECGenericProp::xECSingleInstance::GetSingleInstanceId(ULONG *lpcbInstanceID, LPENTRYID *lppInstanceID)
{
	TRACE_MAPI(TRACE_ENTRY, "IECSingleInstance::GetSingleInstanceId", "");
	METHOD_PROLOGUE_(ECGenericProp , ECSingleInstance);
	HRESULT hr = pThis->GetSingleInstanceId(lpcbInstanceID, (LPSIEID *)lppInstanceID);
	TRACE_MAPI(TRACE_RETURN, "IECSingleInstance::GetSingleInstanceId", "%s", GetMAPIErrorDescription(hr).c_str());
	return hr;
}

HRESULT __stdcall ECGenericProp::xECSingleInstance::SetSingleInstanceId(ULONG cbInstanceID, LPENTRYID lpInstanceID)
{
	TRACE_MAPI(TRACE_ENTRY, "IECSingleInstance::SetSingleInstanceId", "");
	METHOD_PROLOGUE_(ECGenericProp , ECSingleInstance);
	HRESULT hr = pThis->SetSingleInstanceId(cbInstanceID, (LPSIEID)lpInstanceID);
	TRACE_MAPI(TRACE_RETURN, "IECSingleInstance::SetSingleInstanceId", "%s", GetMAPIErrorDescription(hr).c_str());
	return hr;
}
