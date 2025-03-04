
#include "ApiReader.h"

#include "Architecture.h"
#include "SystemInformation.h"
#include "StringConversion.h"
#include "PeParser.h"

#define APIS_ALWAYS_FROM_DISK 0

extern "C" void DebugOutput(_In_ LPCTSTR lpOutputString, ...);
extern "C" void ErrorOutput(_In_ LPCTSTR lpOutputString, ...);

stdext::hash_multimap<DWORD_PTR, ApiInfo *> ApiReader::apiList; //api look up table
std::map<DWORD_PTR, ImportModuleThunk> *  ApiReader::moduleThunkList; //store found apis

DWORD_PTR ApiReader::minApiAddress = (DWORD_PTR)-1;
DWORD_PTR ApiReader::maxApiAddress = 0;

//#define DEBUG_COMMENTS

void ApiReader::readApisFromModuleList()
{
	if (APIS_ALWAYS_FROM_DISK)
	{
		readExportTableAlwaysFromDisk = true;
	}
	else
	{
		readExportTableAlwaysFromDisk = false;
	}

	for (unsigned int i = 0; i < moduleList.size();i++)
	{
		setModulePriority(&moduleList[i]);

		if (moduleList[i].modBaseAddr + moduleList[i].modBaseSize > maxValidAddress)
		{
			maxValidAddress = moduleList[i].modBaseAddr + moduleList[i].modBaseSize;
		}

#ifdef DEBUG_COMMENTS
		DebugOutput("Module parsing: %s", moduleList[i].fullPath);
#endif

		if (!moduleList[i].isAlreadyParsed)
		{
			parseModule(&moduleList[i]);
		}
	}

#ifdef DEBUG_COMMENTS
	DebugOutput("Address Min " PRINTF_DWORD_PTR_FULL " Max " PRINTF_DWORD_PTR_FULL "\nimagebase " PRINTF_DWORD_PTR_FULL " maxValidAddress " PRINTF_DWORD_PTR_FULL, minApiAddress, maxApiAddress, targetImageBase ,maxValidAddress);
#endif
}

void ApiReader::parseModule(ModuleInfo *module)
{
	module->parsing = true;

	//  For CAPE, modules are all in own process, and this call fails for WinSxS modules
	//if (isWinSxSModule(module))
	//{
	//	parseModuleWithMapping(module);
	//}
	//else if (isModuleLoadedInOwnProcess(module)) //this is always ok
	if (isModuleLoadedInOwnProcess(module)) //this is always ok
	{
		parseModuleWithOwnProcess(module);
	}
	else
	{
		if (readExportTableAlwaysFromDisk)
		{
			parseModuleWithMapping(module);
		}
		else
		{
			parseModuleWithProcess(module);
		}
	}
	
	module->isAlreadyParsed = true;
}

void ApiReader::parseModuleWithMapping(ModuleInfo *moduleInfo)
{
	LPVOID fileMapping = 0;
	PIMAGE_NT_HEADERS pNtHeader = 0;
	PIMAGE_DOS_HEADER pDosHeader = 0;

	fileMapping = createFileMappingViewRead(moduleInfo->fullPath);

	if (fileMapping == 0)
		return;

	pDosHeader = (PIMAGE_DOS_HEADER)fileMapping;
	pNtHeader = (PIMAGE_NT_HEADERS)((DWORD_PTR)fileMapping + (DWORD_PTR)(pDosHeader->e_lfanew));

	if (isPeAndExportTableValid(pNtHeader))
	{
		parseExportTable(moduleInfo, pNtHeader, (PIMAGE_EXPORT_DIRECTORY)((DWORD_PTR)fileMapping + pNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress), (DWORD_PTR)fileMapping);
	}


	UnmapViewOfFile(fileMapping);

}

inline bool ApiReader::isApiForwarded(DWORD_PTR rva, PIMAGE_NT_HEADERS pNtHeader)
{
	if ((rva > pNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress) && (rva < (pNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress + pNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size)))
	{
		return true;
	}
	else
	{
		return false;
	}
}

void ApiReader::handleForwardedApi(DWORD_PTR vaStringPointer,char * functionNameParent, DWORD_PTR rvaParent, WORD ordinalParent, ModuleInfo *moduleParent)
{
	size_t dllNameLength = 0;
	WORD ordinal = 0;
	ModuleInfo *module = 0;
	DWORD_PTR vaApi = 0;
	DWORD_PTR rvaApi = 0;
	char dllName[100] = {0};
	WCHAR dllNameW[100] = {0};
	char *fordwardedString = (char *)vaStringPointer;
	char *searchFunctionName = strchr(fordwardedString, '.');
	

	if (!searchFunctionName)
		return;

	dllNameLength = searchFunctionName - fordwardedString;

	if (dllNameLength >= 99)
	{
		return;
	}
	else
	{
		strncpy_s(dllName, fordwardedString, dllNameLength);
	}

	searchFunctionName++;

	if (strchr(searchFunctionName,'#'))
	{
		searchFunctionName++;
		ordinal = (WORD)atoi(searchFunctionName);
	}

	//Since Windows 7
	if (!_strnicmp(dllName, "API-", 4) || !_strnicmp(dllName, "EXT-", 4)) //API_SET_PREFIX_NAME, API_SET_EXTENSION
	{
		/* 
			Info: http://www.nirsoft.net/articles/windows_7_kernel_architecture_changes.html
		*/
		FARPROC address = 0;
		HMODULE hModTemp = GetModuleHandleA(dllName);
		if (hModTemp == 0)
		{
			hModTemp = LoadLibraryA(dllName);
		}

		if (ordinal)
		{
			address = GetProcAddress(hModTemp, (char *)ordinal);
		}
		else
		{
			address = GetProcAddress(hModTemp, searchFunctionName);
		}
#ifdef DEBUG_COMMENTS
		DebugOutput("API_SET_PREFIX_NAME %s %s Module Handle %p address %p",moduleParent->fullPath, dllName, hModTemp, address);
#endif
		if (address != 0)
		{
			addApi(functionNameParent,0, ordinalParent, (DWORD_PTR)address, (DWORD_PTR)address - (DWORD_PTR)hModTemp, true, moduleParent);
		}

		return;
	}

	strcat_s(dllName, ".dll");
	
	StringConversion::ToUTF16(dllName, dllNameW, _countof(dllNameW));
	
	if (!_stricmp(dllName, moduleParent->getFilename()))
	{
		module = moduleParent;
	}
	else
	{
		module = findModuleByName(dllName);
	}

	if (module != 0) // module == 0 -> can be ignored
	{
		/*if ((module->isAlreadyParsed == false) && (module != moduleParent))
		{
			//do API extract
			
			if (module->parsing == true)
			{
				//some stupid circle dependency
				printf("stupid circle dependency %s\n",module->getFilename());
			}
			else
			{
				parseModule(module);
			}
		}*/

		if (ordinal)
		{
			//forwarding by ordinal
			findApiByModuleAndOrdinal(module, ordinal, &vaApi, &rvaApi);
		}
		else
		{
			findApiByModuleAndName(module, searchFunctionName, &vaApi, &rvaApi);
		}

		if (rvaApi == 0)
		{
#ifdef DEBUG_COMMENTS
			DebugOutput("handleForwardedApi: Api not found, this is really BAD! %s",fordwardedString);
#endif
		}
		else
		{
			addApi(functionNameParent,0, ordinalParent, vaApi, rvaApi, true, moduleParent);
		}
	}

}

ModuleInfo * ApiReader::findModuleByName(CHAR *name)
{
	for (unsigned int i = 0; i < moduleList.size(); i++) {
		if (!_stricmp(moduleList[i].getFilename(), name))
		{
			return &moduleList[i];
		}
	}

	return 0;
}

void ApiReader::addApiWithoutName(WORD ordinal, DWORD_PTR va, DWORD_PTR rva,bool isForwarded, ModuleInfo *moduleInfo)
{
	addApi(0, 0, ordinal, va, rva, isForwarded, moduleInfo);
}

void ApiReader::addApi(char *functionName, WORD hint, WORD ordinal, DWORD_PTR va, DWORD_PTR rva, bool isForwarded, ModuleInfo *moduleInfo)
{
	ApiInfo *apiInfo = new ApiInfo();

	if ((functionName != 0) && (strlen(functionName) < _countof(apiInfo->name)))
	{
		strcpy_s(apiInfo->name, functionName);
	}
	else
	{
		apiInfo->name[0] = 0x00;
	}

	apiInfo->ordinal = ordinal;
	apiInfo->isForwarded = isForwarded;
	apiInfo->module = moduleInfo;
	apiInfo->rva = rva;
	apiInfo->va = va;
	apiInfo->hint = hint;

	setMinMaxApiAddress(va);

	moduleInfo->apiList.push_back(apiInfo);

	apiList.insert(API_Pair(va, apiInfo));
}

BYTE * ApiReader::getHeaderFromProcess(ModuleInfo * module)
{
	BYTE *bufferHeader = 0;
	DWORD readSize = 0;

	if (module->modBaseSize < PE_HEADER_BYTES_COUNT)
	{
		readSize = module->modBaseSize;
	}
	else
	{
		readSize = PE_HEADER_BYTES_COUNT;
	}

	bufferHeader = new BYTE[readSize];

	if(!readMemoryFromProcess(module->modBaseAddr, readSize, bufferHeader))
	{
#ifdef DEBUG_COMMENTS
		DebugOutput("getHeaderFromProcess: Error reading header");
#endif
		delete[] bufferHeader;
		return 0;
	}
	else
	{
		return bufferHeader;
	}
}

BYTE * ApiReader::getExportTableFromProcess(ModuleInfo * module, PIMAGE_NT_HEADERS pNtHeader)
{
	DWORD readSize = 0;
	BYTE *bufferExportTable = 0;

	readSize = pNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;

	if (readSize < (sizeof(IMAGE_EXPORT_DIRECTORY) + 8))
	{
		//Something is wrong with the PE Header
#ifdef DEBUG_COMMENTS
		DebugOutput("Something is wrong with the PE Header here Export table size %d", readSize);
#endif
		readSize = sizeof(IMAGE_EXPORT_DIRECTORY) + 100;
	}

	if (readSize)
	{
		bufferExportTable = new BYTE[readSize];

		if (!bufferExportTable)
		{
#ifdef DEBUG_COMMENTS
			DebugOutput("Something is wrong with the PE Header here Export table size %d", readSize);
#endif
			return 0;
		}

		if(!readMemoryFromProcess(module->modBaseAddr + pNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress, readSize, bufferExportTable))
		{
#ifdef DEBUG_COMMENTS
			DebugOutput("getExportTableFromProcess: Error reading export table from process");
#endif
			delete[] bufferExportTable;
			return 0;
		}
		else
		{
			return bufferExportTable;
		}
	}
	else
	{
		return 0;
	}
}

void ApiReader::parseModuleWithProcess(ModuleInfo * module)
{
	PIMAGE_NT_HEADERS pNtHeader = 0;
	PIMAGE_DOS_HEADER pDosHeader = 0;
	BYTE *bufferHeader = 0;
	BYTE *bufferExportTable = 0;
	PeParser peParser(module->modBaseAddr, false);

	if (!peParser.isValidPeFile())
		return;

	pNtHeader = peParser.getCurrentNtHeader();

	if (peParser.hasExportDirectory())
	{
		bufferExportTable = getExportTableFromProcess(module, pNtHeader);

		if(bufferExportTable)
		{
			parseExportTable(module,pNtHeader,(PIMAGE_EXPORT_DIRECTORY)bufferExportTable, (DWORD_PTR)bufferExportTable - pNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
			delete[] bufferExportTable;
		}
	}
}

void ApiReader::parseExportTable(ModuleInfo *module, PIMAGE_NT_HEADERS pNtHeader, PIMAGE_EXPORT_DIRECTORY pExportDir, DWORD_PTR deltaAddress)
{
	DWORD *addressOfFunctionsArray = 0,*addressOfNamesArray = 0;
	WORD *addressOfNameOrdinalsArray = 0;
	char *functionName = 0;
	DWORD_PTR RVA = 0, VA = 0;
	WORD ordinal = 0;
	WORD i = 0, j = 0;
	bool withoutName;

	addressOfFunctionsArray = (DWORD *)((DWORD_PTR)pExportDir->AddressOfFunctions + deltaAddress);
	addressOfNamesArray = (DWORD *)((DWORD_PTR)pExportDir->AddressOfNames + deltaAddress);
	addressOfNameOrdinalsArray = (WORD *)((DWORD_PTR)pExportDir->AddressOfNameOrdinals + deltaAddress);

#ifdef DEBUG_COMMENTS
	DebugOutput("parseExportTable: module %s NumberOfNames %X", module->fullPath, pExportDir->NumberOfNames);
#endif

	for (i = 0; i < pExportDir->NumberOfNames; i++)
	{
		functionName = (char*)(addressOfNamesArray[i] + deltaAddress);
		ordinal = (WORD)(addressOfNameOrdinalsArray[i] + pExportDir->Base);
		RVA = addressOfFunctionsArray[addressOfNameOrdinalsArray[i]];
		VA = addressOfFunctionsArray[addressOfNameOrdinalsArray[i]] + module->modBaseAddr;

#ifdef DEBUG_COMMENTS
		DebugOutput("parseExportTable: api %s ordinal %d imagebase " PRINTF_DWORD_PTR_FULL " RVA " PRINTF_DWORD_PTR_FULL " VA " PRINTF_DWORD_PTR_FULL, functionName, ordinal, module->modBaseAddr, RVA, VA);
#endif
		if (!isApiBlacklisted(functionName))
		{
			if (!isApiForwarded(RVA,pNtHeader))
			{
				addApi(functionName, i, ordinal,VA,RVA,false,module);
			}
			else
			{
#ifdef DEBUG_COMMENTS
				DebugOutput("Forwarded: %s\n",functionName);
#endif
				handleForwardedApi(RVA + deltaAddress,functionName,RVA,ordinal,module);
			}
		}

	}

	/*Exports without name*/
	if (pExportDir->NumberOfNames != pExportDir->NumberOfFunctions)
	{
		for (i = 0; i < pExportDir->NumberOfFunctions; i++)
		{
			withoutName = true;
			for (j = 0; j < pExportDir->NumberOfNames; j++)
			{
				if(addressOfNameOrdinalsArray[j] == i)
				{
					withoutName = false;
					break;
				}
			}
			if (withoutName && addressOfFunctionsArray[i] != 0)
			{
				ordinal = (WORD)(i+pExportDir->Base);
				RVA = addressOfFunctionsArray[i];
				VA = (addressOfFunctionsArray[i] + module->modBaseAddr);


				if (!isApiForwarded(RVA,pNtHeader))
				{
					addApiWithoutName(ordinal,VA,RVA,false,module);
				}
				else
				{
					handleForwardedApi(RVA + deltaAddress,0,RVA,ordinal,module);
				}
			}
		}
	}
}

void ApiReader::findApiByModuleAndOrdinal(ModuleInfo * module, WORD ordinal, DWORD_PTR * vaApi, DWORD_PTR * rvaApi)
{
	findApiByModule(module,0,ordinal,vaApi,rvaApi);
}

void ApiReader::findApiByModuleAndName(ModuleInfo * module, char * searchFunctionName, DWORD_PTR * vaApi, DWORD_PTR * rvaApi)
{
	findApiByModule(module,searchFunctionName,0,vaApi,rvaApi);
}

void ApiReader::findApiByModule(ModuleInfo * module, char * searchFunctionName, WORD ordinal, DWORD_PTR * vaApi, DWORD_PTR * rvaApi)
{
	if (isModuleLoadedInOwnProcess(module))
	{
		HMODULE hModule = GetModuleHandle(module->getFilename());

		if (hModule)
		{
			if (vaApi)
			{
				if (ordinal)
				{
					*vaApi = (DWORD_PTR)GetProcAddress(hModule, (LPCSTR)ordinal);
				}
				else
				{
					*vaApi = (DWORD_PTR)GetProcAddress(hModule, searchFunctionName);
				}

				*rvaApi = (*vaApi) - (DWORD_PTR)hModule;
				*vaApi = (*rvaApi) + module->modBaseAddr;
			}
			else
			{
#ifdef DEBUG_COMMENTS
				DebugOutput("findApiByModule: vaApi == NULL, should never happen %s", searchFunctionName);
#endif
			}
		}
		else
		{
#ifdef DEBUG_COMMENTS
			DebugOutput("findApiByModule: hModule == NULL, should never happen %s", module->getFilename());
#endif
		}
	}
	else
	{
		//search api in extern process
		findApiInProcess(module,searchFunctionName,ordinal,vaApi,rvaApi);
	}
}

bool ApiReader::isModuleLoadedInOwnProcess(ModuleInfo * module)
{
	for (unsigned int i = 0; i < ownModuleList.size(); i++)
	{
		if (!_stricmp(module->fullPath, ownModuleList[i].fullPath))
		{
			return true;
		}
	}
#ifdef DEBUG_COMMENTS
	DebugOutput("isModuleLoadedInOwnProcess returned false: %s\n",module->fullPath);
#endif
	return false;
}

void ApiReader::parseModuleWithOwnProcess( ModuleInfo * module )
{
	PIMAGE_NT_HEADERS pNtHeader = 0;
	PIMAGE_DOS_HEADER pDosHeader = 0;
	HMODULE hModule = GetModuleHandle(module->getFilename());

	if (hModule)
	{
		pDosHeader = (PIMAGE_DOS_HEADER)hModule;
		pNtHeader = (PIMAGE_NT_HEADERS)((DWORD_PTR)hModule + (DWORD_PTR)(pDosHeader->e_lfanew));

		if (isPeAndExportTableValid(pNtHeader))
		{
			parseExportTable(module, pNtHeader, (PIMAGE_EXPORT_DIRECTORY)((DWORD_PTR)hModule + pNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress), (DWORD_PTR)hModule);
		}
	}
	else
	{
#ifdef DEBUG_COMMENTS
		DebugOutput("parseModuleWithOwnProcess: hModule is NULL");
#endif
	}
}

bool ApiReader::isPeAndExportTableValid(PIMAGE_NT_HEADERS pNtHeader)
{
	if (pNtHeader->Signature != IMAGE_NT_SIGNATURE)
	{
		DebugOutput("-> IMAGE_NT_SIGNATURE doesn't match.");
		return false;
	}
	else if ((pNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress == 0) || (pNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size == 0))
	{
		DebugOutput("-> No export table.");
		return false;
	}
	else
	{
		return true;
	}
}

void ApiReader::findApiInProcess(ModuleInfo * module, char * searchFunctionName, WORD ordinal, DWORD_PTR * vaApi, DWORD_PTR * rvaApi)
{
	PIMAGE_NT_HEADERS pNtHeader = 0;
	PIMAGE_DOS_HEADER pDosHeader = 0;
	BYTE *bufferHeader = 0;
	BYTE *bufferExportTable = 0;


	bufferHeader = getHeaderFromProcess(module);

	if (bufferHeader == 0)
		return;

	pDosHeader = (PIMAGE_DOS_HEADER)bufferHeader;
	pNtHeader = (PIMAGE_NT_HEADERS)((DWORD_PTR)bufferHeader + (DWORD_PTR)(pDosHeader->e_lfanew));

	if (isPeAndExportTableValid(pNtHeader))
	{
		bufferExportTable = getExportTableFromProcess(module, pNtHeader);

		if(bufferExportTable)
		{
			findApiInExportTable(module,(PIMAGE_EXPORT_DIRECTORY)bufferExportTable, (DWORD_PTR)bufferExportTable - pNtHeader->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress,searchFunctionName,ordinal,vaApi,rvaApi);
			delete[] bufferExportTable;
		}
	}

	delete[] bufferHeader;
}

bool ApiReader::findApiInExportTable(ModuleInfo *module, PIMAGE_EXPORT_DIRECTORY pExportDir, DWORD_PTR deltaAddress, char * searchFunctionName, WORD ordinal, DWORD_PTR * vaApi, DWORD_PTR * rvaApi)
{
	DWORD *addressOfFunctionsArray = 0,*addressOfNamesArray = 0;
	WORD *addressOfNameOrdinalsArray = 0;
	char *functionName = 0;
	DWORD i = 0, j = 0;

	addressOfFunctionsArray = (DWORD *)((DWORD_PTR)pExportDir->AddressOfFunctions + deltaAddress);
	addressOfNamesArray = (DWORD *)((DWORD_PTR)pExportDir->AddressOfNames + deltaAddress);
	addressOfNameOrdinalsArray = (WORD *)((DWORD_PTR)pExportDir->AddressOfNameOrdinals + deltaAddress);

	if (searchFunctionName)
	{
		for (i = 0; i < pExportDir->NumberOfNames; i++)
		{
			functionName = (char*)(addressOfNamesArray[i] + deltaAddress);

			if (!strcmp(functionName,searchFunctionName))
			{
				*rvaApi = addressOfFunctionsArray[addressOfNameOrdinalsArray[i]];
				*vaApi = addressOfFunctionsArray[addressOfNameOrdinalsArray[i]] + module->modBaseAddr;
				return true;
			}
		}
	}
	else
	{
		for (i = 0; i < pExportDir->NumberOfFunctions; i++)
		{
			if (ordinal == (i+pExportDir->Base))
			{
				*rvaApi = addressOfFunctionsArray[i];
				*vaApi = (addressOfFunctionsArray[i] + module->modBaseAddr);
				return true;
			}
		}
	}

	return false;
}


void ApiReader::setModulePriority(ModuleInfo * module)
{
	const CHAR *moduleFileName = module->getFilename();

	//imports by kernelbase don't exist
	if (!_stricmp(moduleFileName, "kernelbase.dll"))
	{
		module->priority = -1;
	}
	else if (!_stricmp(moduleFileName, "ntdll.dll"))
	{
		module->priority = 0;
	}
	else if (!_stricmp(moduleFileName, "shlwapi.dll"))
	{
		module->priority = 0;
	}
	else if (!_stricmp(moduleFileName, "ShimEng.dll"))
	{
		module->priority = 0;
	}
	else if (!_stricmp(moduleFileName, "kernel32.dll"))
	{
		module->priority = 2;
	}
	else if (!_strnicmp(moduleFileName, "API-", 4) || !_strnicmp(moduleFileName, "EXT-", 4)) //API_SET_PREFIX_NAME, API_SET_EXTENSION
	{
		module->priority = 0;
	}
	else
	{
		module->priority = 1;
	}
}

bool ApiReader::isApiAddressValid(DWORD_PTR virtualAddress)
{
	return apiList.count(virtualAddress) > 0;
}

ApiInfo * ApiReader::getApiByVirtualAddress(DWORD_PTR virtualAddress, bool * isSuspect)
{
	stdext::hash_multimap<DWORD_PTR, ApiInfo *>::iterator it1, it2;
	size_t c = 0;
	size_t countDuplicates = apiList.count(virtualAddress);
	int countHighPriority = 0;
	ApiInfo *apiFound = 0;


	if (countDuplicates == 0)
	{
		return 0;
	}
	else if (countDuplicates == 1)
	{
		//API is 100% correct
		*isSuspect = false;
		it1 = apiList.find(virtualAddress); // Find first match.
		return (ApiInfo *)((*it1).second);
	}
	else
	{
		it1 = apiList.find(virtualAddress); // Find first match.

		//any high priority with a name
		apiFound = getScoredApi(it1,countDuplicates,true,false,false,true,false,false,false,false);

		if (apiFound)
			return apiFound;

		*isSuspect = true;

		//high priority with a name and ansi/unicode name
		apiFound = getScoredApi(it1,countDuplicates,true,true,false,true,false,false,false,false);

		if (apiFound)
			return apiFound;

		//priority 2 with no underline in name
		apiFound = getScoredApi(it1,countDuplicates,true,false,true,false,false,false,true,false);

		if (apiFound)
			return apiFound;

		//priority 1 with a name
		apiFound = getScoredApi(it1,countDuplicates,true,false,false,false,false,true,false,false);

		if (apiFound)
			return apiFound;

		//With a name
		apiFound = getScoredApi(it1,countDuplicates,true,false,false,false,false,false,false,false);

		if (apiFound)
			return apiFound;

		//any with priority, name, ansi/unicode
		apiFound = getScoredApi(it1,countDuplicates,true,true,false,true,false,false,false,true);

		if (apiFound)
			return apiFound;

		//any with priority
		apiFound = getScoredApi(it1,countDuplicates,false,false,false,true,false,false,false,true);

		if (apiFound)
			return apiFound;

		//has prio 0 and name
		apiFound = getScoredApi(it1,countDuplicates,false,false,false,false,true,false,false,true);

		if (apiFound)
			return apiFound;
	}

	//is never reached
	DebugOutput("getApiByVirtualAddress: There is a api resolving bug, VA: " PRINTF_DWORD_PTR_FULL, virtualAddress);
	for (size_t c = 0; c < countDuplicates; c++, it1++)
	{
		apiFound = (ApiInfo *)((*it1).second);
		DebugOutput("-> Possible API: %s ord: %d ", apiFound->name, apiFound->ordinal);
	}
	return (ApiInfo *) 1; 
}

ApiInfo * ApiReader::getScoredApi(stdext::hash_multimap<DWORD_PTR, ApiInfo *>::iterator it1,size_t countDuplicates, bool hasName, bool hasUnicodeAnsiName, bool hasNoUnderlineInName, bool hasPrioDll,bool hasPrio0Dll,bool hasPrio1Dll, bool hasPrio2Dll, bool firstWin )
{
	ApiInfo * foundApi = 0;
	ApiInfo * foundMatchingApi = 0;
	int countFoundApis = 0;
	int scoreNeeded = 0;
	int scoreValue = 0;
	size_t apiNameLength = 0;

	if (hasUnicodeAnsiName || hasNoUnderlineInName)
	{
		hasName = true;
	}

	if (hasName)
		scoreNeeded++;

	if (hasUnicodeAnsiName)
		scoreNeeded++;

	if (hasNoUnderlineInName)
		scoreNeeded++;

	if (hasPrioDll)
		scoreNeeded++;

	if (hasPrio0Dll)
		scoreNeeded++;

	if (hasPrio1Dll)
		scoreNeeded++;

	if (hasPrio2Dll)
		scoreNeeded++;

	for (size_t c = 0; c < countDuplicates; c++, it1++)
	{
		foundApi = (ApiInfo *)((*it1).second);
		scoreValue = 0;

		if (hasName)
		{
			if (foundApi->name[0] != 0x00)
			{
				scoreValue++;

				if (hasUnicodeAnsiName)
				{
					apiNameLength = strlen(foundApi->name);

					if ((foundApi->name[apiNameLength - 1] == 'W') || (foundApi->name[apiNameLength - 1] == 'A'))
					{
						scoreValue++;
					}
				}

				if (hasNoUnderlineInName)
				{
					if (!strrchr(foundApi->name, '_'))
					{
						scoreValue++;
					}
				}
			}
		}

		if (hasPrioDll)
		{
			if (foundApi->module->priority >= 1)
			{
				scoreValue++;
			}
		}

		if (hasPrio0Dll)
		{
			if (foundApi->module->priority == 0)
			{
				scoreValue++;
			}
		}

		if (hasPrio1Dll)
		{
			if (foundApi->module->priority == 1)
			{
				scoreValue++;
			}
		}

		if (hasPrio2Dll)
		{
			if (foundApi->module->priority == 2)
			{
				scoreValue++;
			}
		}


		if (scoreValue == scoreNeeded)
		{
			foundMatchingApi = foundApi;
			countFoundApis++;

			if (firstWin)
			{
				return foundMatchingApi;
			}
		}
	}

	if (countFoundApis == 1)
	{
		return foundMatchingApi;
	}
	else
	{
		return (ApiInfo *)0;
	}

}

void ApiReader::setMinMaxApiAddress(DWORD_PTR virtualAddress)
{
	if (virtualAddress == 0 || virtualAddress == (DWORD_PTR)-1)
		return;

	if (virtualAddress < minApiAddress)
	{
#ifdef DEBUG_COMMENTS
		DebugOutput("virtualAddress %p < minApiAddress %p", virtualAddress, minApiAddress);
#endif
		minApiAddress = virtualAddress - 1;
	}
	if (virtualAddress > maxApiAddress)
	{
		maxApiAddress = virtualAddress + 1;
	}
}

void  ApiReader::readAndParseIAT(DWORD_PTR addressIAT, DWORD sizeIAT, std::map<DWORD_PTR, ImportModuleThunk> &moduleListNew)
{
	moduleThunkList = &moduleListNew;
	BYTE *dataIat = new BYTE[sizeIAT];
	if (readMemoryFromProcess(addressIAT,sizeIAT,dataIat))
	{
		parseIAT(addressIAT,dataIat,sizeIAT);
	}
	else
	{
#ifdef DEBUG_COMMENTS
		DebugOutput("ApiReader::readAndparseIAT: error reading iat " PRINTF_DWORD_PTR_FULL, addressIAT);
#endif
	}

	delete[] dataIat;
}

void ApiReader::parseIAT(DWORD_PTR addressIAT, BYTE * iatBuffer, SIZE_T size)
{
	ApiInfo *apiFound = 0;
	ModuleInfo *module = 0;
	bool isSuspect = false;
	int countApiFound = 0, countApiNotFound = 0;
	DWORD_PTR * pIATAddress = (DWORD_PTR *)iatBuffer;
	SIZE_T sizeIAT = size / sizeof(DWORD_PTR);

	for (SIZE_T i = 0; i < sizeIAT; i++)
	{
#ifdef DEBUG_COMMENTS
		DebugOutput("%08X %08X %d out of %d", addressIAT + (DWORD_PTR)&pIATAddress[i] - (DWORD_PTR)iatBuffer, pIATAddress[i],i,sizeIAT);
#endif
		if (!isInvalidMemoryForIat(pIATAddress[i]))
		{
#ifdef DEBUG_COMMENTS
			DebugOutput("min %p max %p address %p", minApiAddress, maxApiAddress, pIATAddress[i]);
#endif
			if ( (pIATAddress[i] > minApiAddress) && (pIATAddress[i] < maxApiAddress) )
			{
				apiFound = getApiByVirtualAddress(pIATAddress[i], &isSuspect);
#ifdef DEBUG_COMMENTS
				DebugOutput("apiFound %p address %p", apiFound, pIATAddress[i]);
#endif
				if (apiFound == 0)
				{
					DebugOutput("getApiByVirtualAddress: No Api found " PRINTF_DWORD_PTR_FULL, pIATAddress[i]);
				}
				if (apiFound == (ApiInfo *)1)
				{
#ifdef DEBUG_COMMENTS
					DebugOutput("apiFound == (ApiInfo *)1 -> " PRINTF_DWORD_PTR_FULL, pIATAddress[i]);
#endif
				}
				else if (apiFound)
				{
					countApiFound++;
#ifdef DEBUG_COMMENTS
					DebugOutput(PRINTF_DWORD_PTR_FULL " %s %d %s", apiFound->va, apiFound->module->getFilename(), apiFound->ordinal, apiFound->name);
#endif
					if (module != apiFound->module)
					{
						module = apiFound->module;
						addFoundApiToModuleList(addressIAT + (DWORD_PTR)&pIATAddress[i] - (DWORD_PTR)iatBuffer, apiFound, true, isSuspect);
					}
					else
					{
						addFoundApiToModuleList(addressIAT + (DWORD_PTR)&pIATAddress[i] - (DWORD_PTR)iatBuffer, apiFound, false, isSuspect);
					}

				}
				else
				{
					countApiNotFound++;
					addNotFoundApiToModuleList(addressIAT + (DWORD_PTR)&pIATAddress[i] - (DWORD_PTR)iatBuffer, pIATAddress[i]);
#ifdef DEBUG_COMMENTS
					DebugOutput("parseIAT: API not found %08X\n", pIATAddress[i]);
#endif
				}
			}
			else
			{
#ifdef DEBUG_COMMENTS
				DebugOutput("parseIAT: API not found %08X\n", pIATAddress[i]);
#endif
				countApiNotFound++;
				addNotFoundApiToModuleList(addressIAT + (DWORD_PTR)&pIATAddress[i] - (DWORD_PTR)iatBuffer, pIATAddress[i]);
			}
		}

	}

	DebugOutput("IAT parsing finished, found %d valid APIs, missed %d APIs", countApiFound, countApiNotFound);
}

void ApiReader::addFoundApiToModuleList(DWORD_PTR iatAddressVA, ApiInfo * apiFound, bool isNewModule, bool isSuspect)
{
	if (isNewModule)
	{
		addModuleToModuleList(apiFound->module->getFilename(), iatAddressVA - targetImageBase);
	}
	addFunctionToModuleList(apiFound, iatAddressVA, iatAddressVA - targetImageBase, apiFound->ordinal, true, isSuspect);
}

bool ApiReader::addModuleToModuleList(const CHAR * moduleName, DWORD_PTR firstThunk)
{
	ImportModuleThunk module;

	module.firstThunk = firstThunk;
	strcpy_s(module.moduleName, moduleName);

	(*moduleThunkList).insert(std::pair<DWORD_PTR,ImportModuleThunk>(firstThunk,module));

	return true;
}

void ApiReader::addUnknownModuleToModuleList(DWORD_PTR firstThunk)
{
	ImportModuleThunk module;

	module.firstThunk = firstThunk;
	strcpy_s(module.moduleName, "?");

	(*moduleThunkList).insert(std::pair<DWORD_PTR,ImportModuleThunk>(firstThunk,module));
}

bool ApiReader::addFunctionToModuleList(ApiInfo * apiFound, DWORD_PTR va, DWORD_PTR rva, WORD ordinal, bool valid, bool suspect)
{
	ImportThunk import;
	ImportModuleThunk  * module = 0;
	std::map<DWORD_PTR, ImportModuleThunk>::iterator iterator1;

	if ((*moduleThunkList).size() > 1)
	{
		iterator1 = (*moduleThunkList).begin();
		while (iterator1 != (*moduleThunkList).end())
		{
			if (rva >= iterator1->second.firstThunk)
			{
				iterator1++;
				if (iterator1 == (*moduleThunkList).end())
				{
					iterator1--;
					module = &(iterator1->second);
					break;
				}
				else if (rva < iterator1->second.firstThunk)
				{
					iterator1--;
					module = &(iterator1->second);
					break;
				}
			}
			else
			{
#ifdef DEBUG_COMMENTS
				DebugOutput("Error iterator1 != (*moduleThunkList).end()");
#endif
				break;
			}
		}
	}
	else
	{
		iterator1 = (*moduleThunkList).begin();
		module = &(iterator1->second);
	}

	if (!module)
	{
#ifdef DEBUG_COMMENTS
		DebugOutput("ImportsHandling::addFunction module not found rva " PRINTF_DWORD_PTR_FULL, rva);
#endif
		return false;
	}


	import.suspect = suspect;
	import.valid = valid;
	import.va = va;
	import.rva = rva;
	import.apiAddressVA = apiFound->va;
	import.ordinal = ordinal;
	import.hint = (WORD)apiFound->hint;

	strcpy_s(import.moduleName, apiFound->module->getFilename());
	strcpy_s(import.name, apiFound->name);

	module->thunkList.insert(std::pair<DWORD_PTR,ImportThunk>(import.rva, import));

	return true;
}

void ApiReader::clearAll()
{
	minApiAddress = (DWORD_PTR)-1;
	maxApiAddress = 0;

	for ( stdext::hash_map<DWORD_PTR, ApiInfo *>::iterator it = apiList.begin(); it != apiList.end(); ++it )
	{
		delete it->second;
	}
	apiList.clear();

	if (moduleThunkList != 0)
	{
		(*moduleThunkList).clear();
	}
}

bool ApiReader::addNotFoundApiToModuleList(DWORD_PTR iatAddressVA, DWORD_PTR apiAddress)
{
	ImportThunk import;
	ImportModuleThunk  * module = 0;
	std::map<DWORD_PTR, ImportModuleThunk>::iterator iterator1;
	DWORD_PTR rva = iatAddressVA - targetImageBase;

	if ((*moduleThunkList).size() > 0)
	{
		iterator1 = (*moduleThunkList).begin();
		while (iterator1 != (*moduleThunkList).end())
		{
			if (rva >= iterator1->second.firstThunk)
			{
				iterator1++;
				if (iterator1 == (*moduleThunkList).end())
				{
					iterator1--;
					//new unknown module
					if (iterator1->second.moduleName[0] == L'?')
					{
						module = &(iterator1->second);
					}
					else
					{
						addUnknownModuleToModuleList(rva);
						module = &((*moduleThunkList).find(rva)->second);
					}

					break;
				}
				else if (rva < iterator1->second.firstThunk)
				{
					iterator1--;
					module = &(iterator1->second);
					break;
				}
			}
			else
			{
#ifdef DEBUG_COMMENTS
				DebugOutput("Error iterator1 != (*moduleThunkList).end()\r\n");
#endif
				break;
			}
		}
	}
	else
	{
		//new unknown module
		addUnknownModuleToModuleList(rva);
		module = &((*moduleThunkList).find(rva)->second);
	}

	if (!module)
	{
#ifdef DEBUG_COMMENTS
		DebugOutput("ImportsHandling::addFunction module not found rva " PRINTF_DWORD_PTR_FULL,rva);
#endif
		return false;
	}


	import.suspect = true;
	import.valid = false;
	import.va = iatAddressVA;
	import.rva = rva;
	import.apiAddressVA = apiAddress;
	import.ordinal = 0;

	strcpy_s(import.moduleName, "?");
	strcpy_s(import.name, "?");

	module->thunkList.insert(std::pair<DWORD_PTR,ImportThunk>(import.rva, import));

	return true;
}

bool ApiReader::isApiBlacklisted( const char * functionName )
{
	if (SystemInformation::currenOS < WIN_VISTA_32)
	{
		if (!strcmp(functionName, "RestoreLastError"))
		{
			return true;
		}
		else
		{
			return false;
		}
	}
	else
	{
		return false;
	}


	/*#ifdef _WIN64
	else if (SystemInformation::currenOS == WIN_XP_64 && !strcmp(functionName, "DecodePointer"))
	{
		return true;
	}
#endif*/
}

bool ApiReader::isWinSxSModule( ModuleInfo * module )
{
	if (strstr(module->fullPath, "\\WinSxS\\"))
	{
		return true;
	}
	else if (strstr(module->fullPath, "\\winsxs\\"))
	{
		return true;
	}
	else
	{
		return false;
	}
}

bool ApiReader::isInvalidMemoryForIat( DWORD_PTR address )
{
	if (address == 0)
		return true;

   if (address == -1)
	   return true;

   MEMORY_BASIC_INFORMATION memBasic = {0};

   if (VirtualQueryEx(ProcessAccessHelp::hProcess, (LPCVOID)address, &memBasic, sizeof(MEMORY_BASIC_INFORMATION)))
   {
	   if((memBasic.State == MEM_COMMIT) && ProcessAccessHelp::isPageAccessable(memBasic.Protect))
	   {
		   return false;
	   }
	   else
	   {
		   return true;
	   }
   }
   else
   {
	   return true;
   }
}
