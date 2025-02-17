#include "log.h"
#include "misc.h"


HOOKDEF(HRESULT, WINAPI, WMI_Get,
	PVOID		_this,
	LPCWSTR		wszName,
	LONG		lFlags,
	VARIANT*	pVal,
	LONG*		pType,
	LONG*		plFlavor
) {
	HRESULT ret;
	lasterror_t lasterror;

	ret = Old_WMI_Get(_this, wszName, lFlags, pVal, pType, plFlavor);
	get_lasterrors(&lasterror);
	__try {
		if (wcscmp(wszName, L"__GENUS") && wcscmp(wszName, L"__PATH") && wcscmp(wszName, L"__RELPATH") && wcscmp(wszName, L"__SUPERCLASS") && wcscmp(wszName, L"SECURITY_DESCRIPTOR") && wcscmp(wszName, L"__NAMESPACE") && wcscmp(wszName, L"__CLASS")) {
			// Don't log spammy property names
			LOQ_hresult("system", "un", "Name", wszName, "Value", pVal);
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
			;
	}

	set_lasterrors(&lasterror);
	return ret;
}

HOOKDEF_NOTAIL(WINAPI, WMI_ExecQuery,
	PVOID		_this,
	const BSTR	strQueryLanguage,
	const BSTR	strQuery,
	LONG		lFlags,
	PVOID		pCtx,
	PVOID*		ppEnum
) {
	HRESULT ret = 0;
	LOQ_hresult("system", "u", "Query", strQuery);
	return 0;
}

HOOKDEF_NOTAIL(WINAPI, WMI_ExecQueryAsync,
	PVOID		_this,
	const BSTR	strQueryLanguage,
	const BSTR	strQuery,
	long		lFlags,
	PVOID		pCtx,
	PVOID		pResponseHandler
) {
	HRESULT ret = 0;
	LOQ_hresult("system", "u", "Query", strQuery);
	return 0;
}

HOOKDEF_NOTAIL(WINAPI, WMI_ExecMethod,
	PVOID		_this,
	const BSTR	strObjectPath,
	const BSTR	strMethodName,
	long		lFlags,
	PVOID		pCtx,
	PVOID		pInParams,
	PVOID*		ppOutParams,
	PVOID*		ppCallResult
) {
	HRESULT ret = 0;
	LOQ_hresult("system", "uu", "ObjectPath", strObjectPath, "MethodName", strMethodName);
	return 0;
}

HOOKDEF_NOTAIL(WINAPI, WMI_ExecMethodAsync,
	PVOID		_this,
	const BSTR	strObjectPath,
	const BSTR	strMethodName,
	long		lFlags,
	PVOID		pCtx,
	PVOID		pInParams,
	PVOID		pResponseHandler
) {
	HRESULT ret = 0;
	LOQ_hresult("system", "uu", "ObjectPath", strObjectPath, "MethodName", strMethodName);
	return 0;
}

HOOKDEF_NOTAIL(WINAPI, WMI_GetObject,
	PVOID		_this,
	const BSTR	strObjectPath,
	long		lFlags,
	PVOID		pCtx,
	PVOID*		ppObject,
	PVOID*		ppCallResult
) {
	HRESULT ret = 0;
	LOQ_hresult("system", "u", "ObjectPath", strObjectPath);
	return 0;
}

HOOKDEF_NOTAIL(WINAPI, WMI_GetObjectAsync,
	PVOID		_this,
	const BSTR	strObjectPath,
	long		lFlags,
	PVOID		pCtx,
	PVOID		pResultHandler
) {
	HRESULT ret = 0;
	LOQ_hresult("system", "u", "ObjectPath", strObjectPath);
	return 0;
}