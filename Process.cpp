/*
__________________________________________________________________________________________
| _______  _____  __   _ _______ _______ _______                                         |
| |  |  | |     | | \  | |______    |    |_____|                                         |
| |  |  | |_____| |  \_| |______    |    |     |                                         |
|________________________________________________________________________________________|
| Moneta ~ Usermode memory scanner & malware hunter                                      |
|----------------------------------------------------------------------------------------|
| https://www.forrest-orr.net/post/malicious-memory-artifacts-part-ii-bypassing-scanners |
|----------------------------------------------------------------------------------------|
| Author: Forrest Orr - 2020                                                             |
|----------------------------------------------------------------------------------------|
| Contact: forrest.orr@protonmail.com                                                    |
|----------------------------------------------------------------------------------------|
| Licensed under GNU GPLv3                                                               |
|________________________________________________________________________________________|
| ## Features                                                                            |
|                                                                                        |
| ~ Query the memory attributes of any accessible process(es).                           |
| ~ Identify private, mapped and image memory.                                           |
| ~ Correlate regions of memory to their underlying file on disks.                       |
| ~ Identify PE headers and sections corresponding to image memory.                      |
| ~ Identify modified regions of mapped image memory.                                    |
| ~ Identify abnormal memory attributes indicative of malware.                           |
| ~ Create memory dumps of user-specified memory ranges                                  |
| ~ Calculate memory permission/type statistics                                          |
|________________________________________________________________________________________|

*/

#include "StdAfx.h"
#include "FileIo.hpp"
#include "PeFile.hpp"
#include "Processes.hpp"
#include "Memory.hpp"
#include "Interface.hpp"
#include "MemDump.hpp"
#include "Suspicions.hpp"
#include "Signing.h"
#include "PEB.h"

using namespace std;
using namespace Memory;
using namespace Processes;

Process::~Process() {
	if (this->Handle != nullptr) {
		CloseHandle(this->Handle);
	}

	for (vector<Thread*>::const_iterator Itr = this->Threads.begin(); Itr != this->Threads.end(); ++Itr) {
		delete* Itr;
	}

	for (map<uint8_t*, Entity*>::const_iterator Itr = this->Entities.begin(); Itr != this->Entities.end(); ++Itr) {
		if (Itr->second->GetType() == Entity::Type::PE_FILE) {
			delete dynamic_cast<PeVm::Body*>(Itr->second); // This will call the destructors for PE, mapped file and entity all to be called in inheritted order.
		}
		else if (Itr->second->GetType() == Entity::Type::MAPPED_FILE) {
			delete dynamic_cast<MappedFile*>(Itr->second);
		}
		else {
			delete dynamic_cast<Region*>(Itr->second);;
		}
	}
}

Process::Process(uint32_t dwPid) : Pid(dwPid) {
	this->Handle = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, false, dwPid);

	if (this->Handle != nullptr) {
		wchar_t ImageName[MAX_PATH + 1] = { 0 }, DevFilePath[MAX_PATH + 1] = { 0 };

		if (GetModuleBaseNameW(this->Handle, nullptr, ImageName, MAX_PATH + 1) && GetProcessImageFileNameW(this->Handle, DevFilePath, sizeof(DevFilePath))) {
			wchar_t ImageFilePath[MAX_PATH + 1] = { 0 };

			if (FileBase::TranslateDevicePath(DevFilePath, ImageFilePath)) {
				this->Name = wstring(ImageName);
				this->ImageFilePath = wstring(ImageFilePath);
				Interface::Log(VerbosityLevel::Debug, "... mapping address space of PID %d [%ws]\r\n", this->Pid, this->Name.c_str());
				static IsWow64Process_t IsWow64Process = reinterpret_cast<IsWow64Process_t>(GetProcAddress(GetModuleHandleW(L"Kernel32.dll"), "IsWow64Process"));

				if (IsWow64Process != nullptr) {
					BOOL bSelfWow64 = FALSE;

					if (IsWow64Process(GetCurrentProcess(), static_cast<PBOOL>(&bSelfWow64))) {
						if (IsWow64Process(this->Handle, static_cast<PBOOL>(&this->Wow64))) {
							if (this->IsWow64()) {
								Interface::Log(VerbosityLevel::Debug, "... PID %d is Wow64\r\n", this->Pid);
							}
							else {
								if (bSelfWow64) {
									Interface::Log(VerbosityLevel::Debug, "... cannot scan non-Wow64 process from Wow64 Moneta instance\r\n");
									throw 2;
								}
							}
						}
					}
				}
			}
		}

		//
		// CreateToolhelp32Snapshot doesn't work work cross-arhitecture heap enumeration - use the PEB to walk the heaps
		//

		static NtQueryInformationProcess_t NtQueryInformationProcess = reinterpret_cast<NtQueryInformationProcess_t>(GetProcAddress(GetModuleHandleW(L"Ntdll.dll"), "NtQueryInformationProcess"));
		NTSTATUS NtStatus;
		void* RemotePeb = nullptr;
		uint32_t dwPebSize = 0;
		PROCESS_BASIC_INFORMATION Pbi = { 0 };

		if (this->IsWow64()) {
			NtStatus = NtQueryInformationProcess(this->Handle, ProcessWow64Information, &RemotePeb, sizeof(RemotePeb), nullptr);
		}
		else {
			NtStatus = NtQueryInformationProcess(this->Handle, ProcessBasicInformation, &Pbi, sizeof(Pbi), nullptr);

			if (NT_SUCCESS(NtStatus)) {
				RemotePeb = Pbi.PebBaseAddress;
			}
		}

		if (RemotePeb != nullptr) {
			Interface::Log(VerbosityLevel::Debug, "... PEB of 0x%p\r\n", RemotePeb);

			if (this->IsWow64()) {
				PEB32* LocalPeb = new PEB32;
				dwPebSize = sizeof(PEB32);

				if (ReadProcessMemory(this->Handle, RemotePeb, LocalPeb, dwPebSize, nullptr)) {
					uint32_t dwNumberOfHeaps = LocalPeb->NumberOfHeaps;
					uint32_t dwHeapsSize = dwNumberOfHeaps * sizeof(uint32_t);
					uint32_t* Heaps = new uint32_t [dwNumberOfHeaps];

					Interface::Log(VerbosityLevel::Debug, "... successfully read remote PEB to local memory (%d heaps)\r\n", dwNumberOfHeaps);

					if (ReadProcessMemory(this->Handle, reinterpret_cast<void*>(LocalPeb->ProcessHeaps), Heaps, dwHeapsSize, nullptr)) {
						Interface::Log(VerbosityLevel::Debug, "... successfully read remote heaps to local memory.\r\n");

						for (uint32_t dwX = 0; dwX < dwNumberOfHeaps; dwX++) {
							Interface::Log(VerbosityLevel::Debug, "... 0x%08x\r\n", Heaps[dwX]);
							this->Heaps.push_back(reinterpret_cast<void *>(Heaps[dwX]));
						}
					}
				}

				delete LocalPeb;
			}
			else {
				PEB64* LocalPeb = new PEB64;
				dwPebSize = sizeof(PEB64);

				if (ReadProcessMemory(this->Handle, RemotePeb, LocalPeb, dwPebSize, nullptr)) {
					uint32_t dwNumberOfHeaps = LocalPeb->NumberOfHeaps;
					uint32_t dwHeapsSize = dwNumberOfHeaps * sizeof(void*);
					void** Heaps = new void* [dwNumberOfHeaps];

					Interface::Log(VerbosityLevel::Debug, "... successfully read remote PEB to local memory.\r\n");

					if (ReadProcessMemory(this->Handle, reinterpret_cast<void*>(LocalPeb->ProcessHeaps), Heaps, dwHeapsSize, nullptr)) {
						Interface::Log(VerbosityLevel::Debug, "... successfully read remote heaps to local memory.\r\n");

						for (uint32_t dwX = 0; dwX < dwNumberOfHeaps; dwX++) {
							Interface::Log(VerbosityLevel::Debug, "... 0x%p\r\n", Heaps[dwX]);
							this->Heaps.push_back(Heaps[dwX]);
						}
					}
				}

				delete LocalPeb;
			}
		}

		HANDLE hThreadSnap = INVALID_HANDLE_VALUE;
		THREADENTRY32 ThreadEntry;

		if ((hThreadSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)) != INVALID_HANDLE_VALUE) {
			ThreadEntry.dwSize = sizeof(THREADENTRY32);

			if (Thread32First(hThreadSnap, &ThreadEntry)) {
				do {
					if (ThreadEntry.th32OwnerProcessID == this->Pid) {
						try {
							this->Threads.push_back(new Thread(ThreadEntry.th32ThreadID, *this));
						}
						catch (int32_t nError) {
							Interface::Log(VerbosityLevel::Surface, "... failed to query thread information for TID %d in PID %d: cancelling scan of process.\r\n", ThreadEntry.th32ThreadID, this->Pid);
							throw 2;
						}
					}
				} while (Thread32Next(hThreadSnap, &ThreadEntry));
			}

			CloseHandle(hThreadSnap);
			Interface::Log(VerbosityLevel::Debug, "... associated a total of %d threads with the current process.\r\n", this->Threads.size());
		}

		SIZE_T cbRegionSize = 0;
		vector<Subregion*> Subregions;
		vector<Subregion*>::iterator Region;

		for (uint8_t* pBaseAddr = nullptr;; pBaseAddr += cbRegionSize) {
			MEMORY_BASIC_INFORMATION* Mbi = new MEMORY_BASIC_INFORMATION;

			if (VirtualQueryEx(this->Handle, pBaseAddr, Mbi, sizeof(MEMORY_BASIC_INFORMATION)) == sizeof(MEMORY_BASIC_INFORMATION)) {
				cbRegionSize = Mbi->RegionSize;

				if (!Subregions.empty()) { // If the subregion list is empty then there is no region base for comparison
					if (Mbi->AllocationBase != (*Region)->GetBasic()->AllocationBase) {
						this->Entities.insert(make_pair(static_cast<uint8_t *>((*Region)->GetBasic()->AllocationBase), Entity::Create(*this, Subregions)));
						Subregions.clear();
					}
				}

				Subregions.push_back(new Subregion(*this, Mbi));
				Region = Subregions.begin(); // This DOES fix a bug.
			}
			else {
				if (!Subregions.empty()) { // Edge case: new ablock not yet found but finished enumerating sblocks.
					this->Entities.insert(make_pair(static_cast<uint8_t *>((*Region)->GetBasic()->AllocationBase), Entity::Create(*this, Subregions)));
				}

				delete Mbi;
				break;
			}
		}
	}
	else {
		Interface::Log(VerbosityLevel::Debug, "... failed to open handle to PID %d\r\n", this->Pid);
		throw 1;
	}
}

void AlignName(const wchar_t* pOriginalName, wchar_t* pAlignedName, int32_t nAlignTo) { // Make generic and move to interface?
	assert(nAlignTo >= 1);
	assert(wcslen(pOriginalName) <= nAlignTo);

	if (wcslen(pOriginalName)) {
		wcsncpy_s(pAlignedName, (nAlignTo + 1), pOriginalName, nAlignTo);
		for (int32_t nX = wcslen(pAlignedName); nX < nAlignTo; nX++) {
			wcscat_s(pAlignedName, (nAlignTo + 1), L" ");
		}
	}
	else {
		wcscpy_s(pAlignedName, (nAlignTo + 1), L" ");

		for (int32_t nX = 1; nX < nAlignTo; nX++) {
			wcscat_s(pAlignedName, (nAlignTo + 1), L" ");
		}
	}
}

void EnumerateThreads(const wstring Indent, vector<Processes::Thread*> Threads) {
	for (vector<Processes::Thread*>::iterator ThItr = Threads.begin(); ThItr != Threads.end(); ++ThItr) {
		Interface::Log("%wsThread 0x%p [TID 0x%08x]\r\n", Indent.c_str(), (*ThItr)->GetEntryPoint(), (*ThItr)->GetTid());
	}
}

int32_t FilterSuspicions(map <uint8_t*, map<uint8_t*, list<Suspicion *>>>&SuspicionsMap) {
	bool bReWalkMap = false;

	do {
		if (bReWalkMap) {
			bReWalkMap = false; // The re-walk boolean is only set when a suspicion was filtered. Reset it each time this happens.
		}

		/* Concept ~ Walk the map and search through the suspicion list corresponding to each sblock.
		             When a suspicion is filtered, remove it from the list, and remove the sblock map
					 entry if it was the only suspicion in its list. If the ablock map only had the
					 one sblock map entry, then remove the ablock map entry as well. Walk the list
					 again now that the map has updated, and repeat the process until there are no
					 filterable suspicions remaining
		
		*/
		for (map <uint8_t*, map<uint8_t*, list<Suspicion *>>>::const_iterator AbMapItr = SuspicionsMap.begin(); !bReWalkMap && AbMapItr != SuspicionsMap.end(); ++AbMapItr) {
			/*
			Region map -> Key [Allocation base]
							-> Suspicions map -> Key [Subregion address]
												   -> Suspicions list
			*/
			map < uint8_t*, list<Suspicion *>>& RefSbMap = SuspicionsMap.at(AbMapItr->first);
			int32_t nSbIndex = 0;

			for (map<uint8_t*, list<Suspicion *>>::const_iterator SbMapItr = AbMapItr->second.begin(); !bReWalkMap && SbMapItr != AbMapItr->second.end(); ++SbMapItr, nSbIndex++) {
				list<Suspicion *>& RefSuspList = RefSbMap.at(SbMapItr->first);
				list<Suspicion *>::const_iterator SuspItr = SbMapItr->second.begin();

				for (int32_t nSuspIndex = 0; !bReWalkMap && SuspItr != SbMapItr->second.end(); ++SuspItr, nSuspIndex++) {
					switch ((*SuspItr)->GetType()) {
					case Suspicion::Type::XPRV: {
						if (((*SuspItr)->GetBlock()->GetFlags() & MEMORY_SUBREGION_FLAG_HEAP)&&false) {
							bReWalkMap = true;
							RefSuspList.erase(SuspItr);

							if (!RefSuspList.size()) {
								//
								// Erase the suspicion list from the sblock map and then erase the sblock map from the ablock map. Finalize by removing the ablock map from the suspicion map itself.
								//

								RefSbMap.erase(SbMapItr);

								if (!RefSbMap.size()) {
									SuspicionsMap.erase(AbMapItr); // Will this cause a bug if multiple suspicions are erased in one call to this function?
								}
							}
						}

						break;
					}
					case Suspicion::Type::MISSING_PEB_MODULE: {
						/* Filter cases for missing PEB modules:
							 ~ Signed metadata PEs. These appear in the C:\Windows\System32\WinMetadata folder with the .winmd extension. They've also been noted to appear in WindpwsApps, SystemApps and others.

							   0x000000000F3E0000:0x0009e000 | Executable image | C:\Windows\System32\WinMetadata\Windows.UI.winmd | Missing PEB module
							   0x000000000F3E0000:0x0009e000 | R        | Header   | 0x00000000
							   0x000000000F3E0000:0x0009e000 | R        | .text    | 0x00000000
							   0x000000000F3E0000:0x0009e000 | R        | .rsrc    | 0x00000000
						*/

						const PeVm::Body* PeEntity = dynamic_cast<const PeVm::Body*>((*SuspItr)->GetParentObject());

						if (PeEntity->IsSigned()) {
							static const wchar_t* pWinmbExt = L".winmd";

							if (_wcsicmp(PeEntity->GetFileBase()->GetPath().c_str() + PeEntity->GetFileBase()->GetPath().length() - wcslen(pWinmbExt), pWinmbExt) == 0) {
								if (PeEntity->GetPe() != nullptr && PeEntity->GetPe()->GetEntryPoint() == 0) {
									bReWalkMap = true;
									RefSuspList.erase(SuspItr);

									if (!RefSuspList.size()) {
										//
										// Erase the suspicion list from the sblock map and then erase the sblock map from the ablock map. Finalize by removing the ablock map from the suspicion map itself.
										//

										RefSbMap.erase(SbMapItr);

										if (!RefSbMap.size()) {
											SuspicionsMap.erase(AbMapItr); // Will this cause a bug if multiple suspicions are erased in one call to this function?
										}
									}
								}
							}
						}

						break;
					}
					}
				}
			}
		}
	} while (bReWalkMap);

	return 0;
}

int32_t AppendOverlapSuspicion(map<uint8_t*, list<Suspicion *>>* Suspicions, uint8_t *pSbAddress, bool bEntityTop) {
	int32_t nCount = 0;

	if (Suspicions != nullptr && Suspicions->count(pSbAddress)) {
		list<Suspicion *>& SuspicionsList = Suspicions->at(pSbAddress);

		for (list<Suspicion *>::const_iterator SuspItr = SuspicionsList.begin(); SuspItr != SuspicionsList.end(); ++SuspItr) {
			if (bEntityTop == (*SuspItr)->IsFullEntitySuspicion()) {
				Interface::Log(" | ");
				Interface::Log(ConsoleColor::Red, "%ws", (*SuspItr)->GetDescription().c_str());
				nCount++;
			}
		}
	}

	return nCount;
}

int32_t AppendSubregionAttributes(Subregion *Sbr) {
	int32_t nCount = 0;

	if ((Sbr->GetFlags() & MEMORY_SUBREGION_FLAG_HEAP)) {
		Interface::Log(" | ");
		Interface::Log(ConsoleColor::Yellow, "Heap");
		nCount++;
	}

	if ((Sbr->GetFlags() & MEMORY_SUBREGION_FLAG_TEB)) {
		Interface::Log(" | ");
		Interface::Log(ConsoleColor::Yellow, "TEB");
		nCount++;
	}

	if ((Sbr->GetFlags() & MEMORY_SUBREGION_FLAG_STACK)) {
		Interface::Log(" | ");
		Interface::Log(ConsoleColor::Yellow, "Stack");
		nCount++;
	}

	return nCount;
}
int32_t SubEntitySuspCount(map<uint8_t*, list<Suspicion*>>* Suspicions, uint8_t* pSbAddress) {
	int32_t nCount = 0;

	if (Suspicions != nullptr && Suspicions->count(pSbAddress)) {
		list<Suspicion*>& SuspicionsList = Suspicions->at(pSbAddress);

		for (list<Suspicion*>::const_iterator SuspItr = SuspicionsList.begin(); SuspItr != SuspicionsList.end(); ++SuspItr) {
			if (!(*SuspItr)->IsFullEntitySuspicion()) {
				nCount++;
			}
		}
	}

	return nCount;
}

bool Process::DumpBlock(MemDump &ProcDmp, const MEMORY_BASIC_INFORMATION *Mbi, wstring Indent) {
	wchar_t DumFilePath[MAX_PATH + 1] = { 0 };

	if (Mbi->State == MEM_COMMIT) {
		if (ProcDmp.Create(Mbi, DumFilePath, MAX_PATH + 1)) {
			Interface::Log("%ws~ Memory dumped to %ws\r\n", Indent.c_str(), DumFilePath);
			return true;
		}
		else {
			Interface::Log("%ws~ Memory dump failed.\r\n", Indent.c_str());
			return false;
		}
	}
}
/* Process memory enumeration
	1. Loop entities to build suspicions list
	2. Filter suspicions
	3. Loop entities for enumeration if:
	   mselect == process
	   mselect == sblock and this eneity contains the sblock
	   mselect == suspicious and there is 1 or more suspicions
	4. Show the process if it has not been shown before
	5. Display entity info (exe image, private, mapped + total size) ALWAYS (criteria already applied going into loop) along with suspicions (if any)
	5. For PEs, loop sblocks/sections. Enum if:
		mselect == process
		mselect == sblock && sblock == current, or the  "from base" option is set
		or mselect == suspicious and the current sblock has a suspicion or the  "from base" option is set
	6. Dump the current sblock based on the same criteria as above but ONLY if the "from base" option is not set.
	7. Dump the entire PE entity if it met the initial enum criteria and "from base" option is set
	8. For private/mapped loop sblocks and enum if:
		mselect == process
		mselect == sblock && sblock == current, or the  "from base" option is set
		or mselect == suspicious and the current sblock has a suspicion or the  "from base" option is set
	9. Dump the current sblock based on the same criteria as above but ONLY if the "from base" option is not set.
	10. Dump the entire entity if it met the initial enum criteria and "from base" option is set
*/

vector<Subregion*> Process::Enumerate(uint64_t qwOptFlags, MemorySelection_t MemSelectType, const uint8_t *pSelectAddress) {
	bool bShownProc = false;
	MemDump ProcDmp(this->Handle, this->Pid);
	wstring_convert<codecvt_utf8_utf16<wchar_t>> UnicodeConverter;
	map <uint8_t*, map<uint8_t*, list<Suspicion *>>> SuspicionsMap; // More efficient to only filter this map once. Currently filtering it for every single entity
	vector<Subregion*> SelectedSbrs;

	//
	// Build suspicions list for following memory selection and apply filters to it.
	//

	for (map<uint8_t*, Entity*>::const_iterator Itr = this->Entities.begin(); Itr != this->Entities.end(); ++Itr) {
		Suspicion::InspectEntity(*this, *Itr->second, SuspicionsMap);
	}

	if (SuspicionsMap.size()) {
		FilterSuspicions(SuspicionsMap);
	}

	//
	// Display information on each selected sblock and/or entity within the process address space
	//

	for (map<uint8_t*, Entity*>::const_iterator Itr = this->Entities.begin(); Itr != this->Entities.end(); ++Itr) {
		auto AbMapItr = SuspicionsMap.find(static_cast<unsigned char *>(const_cast<void*>(Itr->second->GetStartVa()))); // An iterator into the main ablock map which points to the entry for the sb map.
		map<uint8_t*, list<Suspicion *>>* SbrMap = nullptr;

		if (AbMapItr != SuspicionsMap.end()) {
			SbrMap = &SuspicionsMap.at(static_cast<unsigned char*>(const_cast<void*>(Itr->second->GetStartVa())));
		}

		if (MemSelectType == MemorySelection_t::All ||
			(MemSelectType == MemorySelection_t::Block && ((pSelectAddress >= Itr->second->GetStartVa()) && (pSelectAddress < Itr->second->GetEndVa()))) ||
			(MemSelectType == MemorySelection_t::Suspicious && AbMapItr != SuspicionsMap.end())) {

			//
			// Display process and/or entity information: the criteria has already been met for this to be done without further checks
			//

			if (!bShownProc) {
				Interface::Log("\r\n");
				Interface::Log(ConsoleColor::Turquoise, "%ws", this->Name.c_str());
				Interface::Log(" : ");
				Interface::Log(ConsoleColor::Turquoise, "%d", this->GetPid());
				Interface::Log(" : ");
				Interface::Log(ConsoleColor::Turquoise, "%ws", this->IsWow64() ? L"Wow64" : L"x64");
				Interface::Log(" : ");
				Interface::Log(ConsoleColor::Turquoise, "%ws\r\n", this->ImageFilePath.c_str());
				bShownProc = true;
			}

			if (Itr->second->GetSubregions().front()->GetBasic()->State != MEM_FREE) {
				Interface::Log("  0x%p:0x%08x   ", Itr->second->GetStartVa(), Itr->second->GetEntitySize());
			}

			if (Itr->second->GetType() == Entity::Type::PE_FILE) {
				PeVm::Body* PeEntity = dynamic_cast<PeVm::Body*>(Itr->second);
				Interface::Log("| ");

				if (PeEntity->IsNonExecutableImage()) {
					Interface::Log(ConsoleColor::Gold, "Unexecutable image  ");
				}
				else {
					Interface::Log(ConsoleColor::Gold, "Executable image    ");
				}

				Interface::Log("| %ws", PeEntity->GetFileBase()->GetPath().c_str());
			}
			else if (Itr->second->GetType() == Entity::Type::MAPPED_FILE) {
				Interface::Log("| ");
				Interface::Log(ConsoleColor::Gold, "Mapped");
				Interface::Log("   | %ws", dynamic_cast<MappedFile*>(Itr->second)->GetFileBase()->GetPath().c_str());
			}
			else {
				if (Itr->second->GetSubregions().front()->GetBasic()->Type == MEM_PRIVATE) {
					Interface::Log("| ");
					Interface::Log(ConsoleColor::Gold, "Private");
				}
				else {
					continue;
				}
			}

			//
			// Display suspicions associated with the entity, if the current entity has any suspicions associated with it
			//

			//AppendSubregionAttributes(Itr->second->GetSubregions().front());
			AppendOverlapSuspicion(SbrMap, static_cast<uint8_t*>(const_cast<void *>(Itr->second->GetStartVa())), true);
			Interface::Log("\r\n");

			if (Interface::GetVerbosity() == VerbosityLevel::Detail) {
				if (Itr->second->GetType() == Entity::Type::PE_FILE) {
					PeVm::Body* PeEntity = dynamic_cast<PeVm::Body*>(Itr->second);

					Interface::Log("  |__ Mapped file base: 0x%p\r\n", PeEntity->GetStartVa());
					Interface::Log("    | Mapped file size: %d\r\n", PeEntity->GetEntitySize());
					Interface::Log("    | Mapped file path: %ws\r\n", PeEntity->GetFileBase()->GetPath().c_str());
					Interface::Log("    | Size of image: %d\r\n", PeEntity->GetImageSize());
					Interface::Log("    | Non-executable: %ws\r\n", PeEntity->IsNonExecutableImage() ? L"yes" : L"no");
					Interface::Log("    | Partially mapped: %ws\r\n", PeEntity->IsPartiallyMapped() ? L"yes" : L"no");
					Interface::Log("    | Signed: %ws [%ws]\r\n", PeEntity->IsSigned() ? L"yes" : L"no", TranslateSigningType(PeEntity->GetSisningType()));
					Interface::Log("    | Signing level: %ws\r\n", TranslateSigningLevel(PeEntity->GetSigningLevel()));
					Interface::Log("    |__ PEB module");

					if (PeEntity->GetPebModule().Exists()) {
						Interface::Log("\r\n");
						Interface::Log("      | Name: %ws\r\n", PeEntity->GetPebModule().GetName().c_str());
						Interface::Log("      | Image base: 0x%p\r\n", PeEntity->GetPebModule().GetBase());
						Interface::Log("      | Image size: %d\r\n", PeEntity->GetPebModule().GetSize());
						Interface::Log("      | Entry point: 0x%p\r\n", PeEntity->GetPebModule().GetEntryPoint());
						Interface::Log("      | Image file path: %ws\r\n", PeEntity->GetPebModule().GetPath().c_str());
					}
					else {
						Interface::Log(" (missing)\r\n");
					}
				}
				else if (Itr->second->GetType() == Entity::Type::MAPPED_FILE) {
					Interface::Log("  |__ Mapped file base: 0x%p\r\n", Itr->second->GetStartVa());
					Interface::Log("    | Mapped file size: %d\r\n", Itr->second->GetEntitySize());
					Interface::Log("    | Mapped file path: %ws\r\n", dynamic_cast<MappedFile*>(Itr->second)->GetFileBase()->GetPath().c_str());
				}
				/*
				if (Itr->second->GetRegionInfo() != nullptr) {
					// Due to flag inconsistency between architectures and different Windows version MEMORY_REGION_INFORMATION has been excluded
				}
				*/
			}

			//
			// Display the section/sblock information associated with this eneity provided it meets the selection criteria
			//

			vector<Subregion*> Subregions = Itr->second->GetSubregions();

			for (vector<Subregion*>::iterator SbrItr = Subregions.begin(); SbrItr != Subregions.end(); ++SbrItr) {
				if (MemSelectType == MemorySelection_t::All ||
					(MemSelectType == MemorySelection_t::Block && (pSelectAddress == (*SbrItr)->GetBasic()->BaseAddress || (qwOptFlags & PROCESS_ENUM_FLAG_FROM_BASE))) ||
					(MemSelectType == MemorySelection_t::Suspicious && ((qwOptFlags & PROCESS_ENUM_FLAG_FROM_BASE) || 
																		  (SbrMap != nullptr &&
																		   SbrMap->count(static_cast<uint8_t *>((*SbrItr)->GetBasic()->BaseAddress))) &&
																		   SubEntitySuspCount(SbrMap, static_cast<uint8_t *>((*SbrItr)->GetBasic()->BaseAddress)) > 0))) {
					wchar_t AlignedAttribDesc[9] = { 0 };

					AlignName(Subregion::AttribDesc((*SbrItr)->GetBasic()), AlignedAttribDesc, 8);

					if (Itr->second->GetType() == Entity::Type::PE_FILE && !dynamic_cast<PeVm::Body*>(Itr->second)->GetFileBase()->IsPhantom()) {
						//
						// Generate a list of all sections overlapping with this sblock and display them all. A typical example is a +r sblock at the end of the PE which encompasses all consecutive readonly sections ie. .rdata, .rsrc, .reloc
						//

						vector<PeVm::Section*> OverlapSections = dynamic_cast<PeVm::Body*>(Itr->second)->FindOverlapSect(*(*SbrItr));

						if (OverlapSections.empty()) {
							Interface::Log("    0x%p:0x%08x | %ws | ?        | 0x%08x", (*SbrItr)->GetBasic()->BaseAddress, (*SbrItr)->GetBasic()->RegionSize, AlignedAttribDesc, (*SbrItr)->GetPrivateSize());
							AppendSubregionAttributes(*SbrItr);
							AppendOverlapSuspicion(SbrMap, static_cast<uint8_t *>((*SbrItr)->GetBasic()->BaseAddress), false);
							Interface::Log("\r\n");
						}
						else{
							for (vector<PeVm::Section*>::const_iterator SectItr = OverlapSections.begin(); SectItr != OverlapSections.end(); ++SectItr) {
								wchar_t AlignedSectName[9] = { 0 };
								char AnsiSectName[9];

								strncpy_s(AnsiSectName, 9, (char*)(*SectItr)->GetHeader()->Name, 8);
								wstring UnicodeSectName = UnicodeConverter.from_bytes(AnsiSectName);
								AlignName(static_cast<const wchar_t*>(UnicodeSectName.c_str()), AlignedSectName, 8);

								Interface::Log("    0x%p:0x%08x | %ws | %ws | 0x%08x", (*SbrItr)->GetBasic()->BaseAddress, (*SbrItr)->GetBasic()->RegionSize, AlignedAttribDesc, AlignedSectName, (*SbrItr)->GetPrivateSize());
								AppendSubregionAttributes(*SbrItr);
								AppendOverlapSuspicion(SbrMap, static_cast<uint8_t *>((*SbrItr)->GetBasic()->BaseAddress), false);
								Interface::Log("\r\n");

							}
						}
					}
					else {
						Interface::Log("    0x%p:0x%08x | %ws | 0x%08x", (*SbrItr)->GetBasic()->BaseAddress, (*SbrItr)->GetBasic()->RegionSize, AlignedAttribDesc, (*SbrItr)->GetPrivateSize());
						AppendSubregionAttributes(*SbrItr);
						AppendOverlapSuspicion(SbrMap, static_cast<uint8_t *>((*SbrItr)->GetBasic()->BaseAddress), false);
						Interface::Log("\r\n");
					}

					if (Interface::GetVerbosity() == VerbosityLevel::Detail) {
						Interface::Log("    |__ Base address: 0x%p\r\n", (*SbrItr)->GetBasic()->BaseAddress);
						Interface::Log("      | Size: 0x%d\r\n", (*SbrItr)->GetBasic()->RegionSize);
						Interface::Log("      | Permissions: %ws\r\n", Subregion::ProtectSymbol((*SbrItr)->GetBasic()->Protect));
						Interface::Log("      | Type: %ws\r\n", Subregion::TypeSymbol((*SbrItr)->GetBasic()->Type));
						Interface::Log("      | State: %ws\r\n", Subregion::StateSymbol((*SbrItr)->GetBasic()->State));
						Interface::Log("      | Allocation base: 0x%p\r\n", (*SbrItr)->GetBasic()->AllocationBase);
						Interface::Log("      | Allocation permissions: %ws\r\n", Subregion::ProtectSymbol((*SbrItr)->GetBasic()->AllocationProtect));
						Interface::Log("      | Private size: %d [%d pages]\r\n", (*SbrItr)->GetPrivateSize(), (*SbrItr)->GetPrivateSize() / 0x1000);
					}

					EnumerateThreads(L"      ", (*SbrItr)->GetThreads());

					if ((qwOptFlags & PROCESS_ENUM_FLAG_MEMDUMP)) {
						if (!(qwOptFlags & PROCESS_ENUM_FLAG_FROM_BASE)) {
							this->DumpBlock(ProcDmp, (*SbrItr)->GetBasic(), L"      ");
						}
					}

					SelectedSbrs.push_back(*SbrItr);
				}
			}

			if ((qwOptFlags & PROCESS_ENUM_FLAG_MEMDUMP)) {
				if ((qwOptFlags & PROCESS_ENUM_FLAG_FROM_BASE)) {
					if (Entity::Dump(ProcDmp, *Itr->second)) {
						Interface::Log("      ~ Generated full region dump at 0x%p\r\n", Itr->second->GetStartVa());
					}
					else {
						Interface::Log("      ~ Failed to generate full region dump at 0x%p\r\n", Itr->second->GetStartVa());
					}
				}
			}
		}
	}

	return SelectedSbrs;
}
