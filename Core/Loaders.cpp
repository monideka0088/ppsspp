// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <algorithm>
#include <cstdio>

#include "Common/File/FileUtil.h"
#include "Common/File/Path.h"
#include "Common/StringUtils.h"
#include "Core/FileLoaders/CachingFileLoader.h"
#include "Core/FileLoaders/DiskCachingFileLoader.h"
#include "Core/FileLoaders/HTTPFileLoader.h"
#include "Core/FileLoaders/LocalFileLoader.h"
#include "Core/FileLoaders/RetryingFileLoader.h"
#include "Core/FileSystems/MetaFileSystem.h"
#include "Core/PSPLoaders.h"
#include "Core/MemMap.h"
#include "Core/Loaders.h"
#include "Core/System.h"
#include "Core/ELF/PBPReader.h"
#include "Core/ELF/ParamSFO.h"

static std::map<std::string, std::unique_ptr<FileLoaderFactory>> factories;

void RegisterFileLoaderFactory(std::string prefix, std::unique_ptr<FileLoaderFactory> factory) {
	factories[prefix] = std::move(factory);
}

FileLoader *ConstructFileLoader(const Path &filename) {
	if (filename.Type() == PathType::HTTP) {
		FileLoader *baseLoader = new RetryingFileLoader(new HTTPFileLoader(filename));
		// For headless, avoid disk caching since it's usually used for tests that might mutate.
		if (!PSP_CoreParameter().headLess) {
			baseLoader = new DiskCachingFileLoader(baseLoader);
		}
		return new CachingFileLoader(baseLoader);
	}

	for (auto &iter : factories) {
		if (startsWith(filename.ToString(), iter.first)) {
			return iter.second->ConstructFileLoader(filename);
		}
	}
	return new LocalFileLoader(filename);
}

// TODO : improve, look in the file more
IdentifiedFileType Identify_File(FileLoader *fileLoader, std::string *errorString) {
	*errorString = "";
	if (fileLoader == nullptr) {
		*errorString = "Invalid fileLoader";
		return IdentifiedFileType::ERROR_IDENTIFYING;
	}
	if (fileLoader->GetPath().size() == 0) {
		*errorString = "Invalid filename " + fileLoader->GetPath().ToString();
		return IdentifiedFileType::ERROR_IDENTIFYING;
	}

	if (!fileLoader->Exists()) {
		*errorString = "IdentifyFile: File doesn't exist" + fileLoader->GetPath().ToString();
		return IdentifiedFileType::ERROR_IDENTIFYING;
	}

	std::string extension = fileLoader->GetPath().GetFileExtension();
	if (extension == ".iso") {
		// may be a psx iso, they have 2352 byte sectors. You never know what some people try to open
		if ((fileLoader->FileSize() % 2352) == 0) {
			unsigned char sync[12];
			fileLoader->ReadAt(0, 12, sync);

			// each sector in a mode2 image starts with these 12 bytes
			if (memcmp(sync,"\x00\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x00", 12) == 0) {
				*errorString = "ISO in Mode 2: Not a PSP game";
				return IdentifiedFileType::ISO_MODE2;
			}

			// maybe it also just happened to have that size, let's assume it's a PSP ISO and error out later if it's not.
		}
		return IdentifiedFileType::PSP_ISO;
	} else if (extension == ".cso") {
		return IdentifiedFileType::PSP_ISO;
	} else if (extension == ".ppst") {
		return IdentifiedFileType::PPSSPP_SAVESTATE;
	} else if (extension == ".ppdmp") {
		char data[8]{};
		fileLoader->ReadAt(0, 8, data);
		if (memcmp(data, "PPSSPPGE", 8) == 0) {
			return IdentifiedFileType::PPSSPP_GE_DUMP;
		}
	}

	// First, check if it's a directory with an EBOOT.PBP in it.
	if (fileLoader->IsDirectory()) {
		Path filename = fileLoader->GetPath();
		if (filename.size() > 4) {
			// Check for existence of EBOOT.PBP, as required for "Directory games".
			if (File::Exists(filename / "EBOOT.PBP")) {
				return IdentifiedFileType::PSP_PBP_DIRECTORY;
			}

			// check if it's a disc directory
			if (File::Exists(filename / "PSP_GAME")) {
				return IdentifiedFileType::PSP_DISC_DIRECTORY;
			}

			// Not that, okay, let's guess it's a savedata directory if it has a param.sfo...
			if (File::Exists(filename / "PARAM.SFO")) {
				return IdentifiedFileType::PSP_SAVEDATA_DIRECTORY;
			}
		}

		return IdentifiedFileType::NORMAL_DIRECTORY;
	}

	u32_le id;

	size_t readSize = fileLoader->ReadAt(0, 4, 1, &id);
	if (readSize != 1) {
		*errorString = "Failed to read identification bytes";
		return IdentifiedFileType::ERROR_IDENTIFYING;
	}

	u32_le psar_offset = 0, psar_id = 0;
	u32 _id = id;
	if (!memcmp(&_id, "PK\x03\x04", 4) || !memcmp(&_id, "PK\x05\x06", 4) || !memcmp(&_id, "PK\x07\x08", 4)) {
		return IdentifiedFileType::ARCHIVE_ZIP;
	} else if (!memcmp(&_id, "\x00PBP", 4)) {
		fileLoader->ReadAt(0x24, 4, 1, &psar_offset);
		fileLoader->ReadAt(psar_offset, 4, 1, &psar_id);
	} else if (!memcmp(&_id, "Rar!", 4)) {
		return IdentifiedFileType::ARCHIVE_RAR;
	}

	if (id == 'FLE\x7F') {
		Path filename = fileLoader->GetPath();
		// There are a few elfs misnamed as pbp (like Trig Wars), accept that.
		if (extension == ".plf" || strstr(filename.GetFilename().c_str(), "BOOT.BIN") ||
			extension == ".elf" || extension == ".prx" || extension == ".pbp") {
			return IdentifiedFileType::PSP_ELF;
		}
		return IdentifiedFileType::UNKNOWN_ELF;
	} else if (id == 'PBP\x00') {
		// Do this PS1 eboot check FIRST before checking other eboot types.
		// It seems like some are malformed and slip through the PSAR check below.
		PBPReader pbp(fileLoader);
		if (pbp.IsValid() && !pbp.IsELF()) {
			std::vector<u8> sfoData;
			if (pbp.GetSubFile(PBP_PARAM_SFO, &sfoData)) {
				ParamSFOData paramSFO;
				paramSFO.ReadSFO(sfoData);
				// PS1 Eboots are supposed to use "ME" as their PARAM SFO category.
				// If they don't, and they're still malformed (e.g. PSISOIMG0000 isn't found), there's nothing we can do.
				if (paramSFO.GetValueString("CATEGORY") == "ME")
					return IdentifiedFileType::PSP_PS1_PBP;
			}
		}

		if (psar_id == 'MUPN') {
			return IdentifiedFileType::PSP_ISO_NP;
		}
		// PS1 PSAR begins with "PSISOIMG0000"
		if (psar_id == 'SISP') {
			return IdentifiedFileType::PSP_PS1_PBP;
		}

		// Let's check if we got pointed to a PBP within such a directory.
		// If so we just move up and return the directory itself as the game.
		// If loading from memstick...
		if (fileLoader->GetPath().FilePathContains("PSP/GAME/")) {
			return IdentifiedFileType::PSP_PBP_DIRECTORY;
		}
		return IdentifiedFileType::PSP_PBP;
	} else if (extension == ".pbp") {
		ERROR_LOG(LOADER, "A PBP with the wrong magic number?");
		return IdentifiedFileType::PSP_PBP;
	} else if (extension == ".bin") {
		return IdentifiedFileType::UNKNOWN_BIN;
	} else if (extension == ".zip") {
		return IdentifiedFileType::ARCHIVE_ZIP;
	} else if (extension == ".rar") {
		return IdentifiedFileType::ARCHIVE_RAR;
	} else if (extension == ".r00") {
		return IdentifiedFileType::ARCHIVE_RAR;
	} else if (extension == ".r01") {
		return IdentifiedFileType::ARCHIVE_RAR;
	} else if (extension == ".7z") {
		return IdentifiedFileType::ARCHIVE_7Z;
	}
	return IdentifiedFileType::UNKNOWN;
}

FileLoader *ResolveFileLoaderTarget(FileLoader *fileLoader) {
	std::string errorString;
	IdentifiedFileType type = Identify_File(fileLoader, &errorString);
	if (type == IdentifiedFileType::PSP_PBP_DIRECTORY) {
		const Path ebootFilename = ResolvePBPFile(fileLoader->GetPath());
		if (ebootFilename != fileLoader->GetPath()) {
			// Switch fileLoader to the actual EBOOT.
			delete fileLoader;
			fileLoader = ConstructFileLoader(ebootFilename);
		}
	}
	return fileLoader;
}

Path ResolvePBPDirectory(const Path &filename) {
	if (filename.GetFilename() == "EBOOT.PBP") {
		return filename.NavigateUp();
	} else {
		return filename;
	}
}

Path ResolvePBPFile(const Path &filename) {
	if (filename.GetFilename() != "EBOOT.PBP") {
		return filename / "EBOOT.PBP";
	} else {
		return filename;
	}
}

bool LoadFile(FileLoader **fileLoaderPtr, std::string *error_string) {
	FileLoader *&fileLoader = *fileLoaderPtr;
	// Note that this can modify filename!
	IdentifiedFileType type = Identify_File(fileLoader, error_string);
	switch (type) {
	case IdentifiedFileType::PSP_PBP_DIRECTORY:
		{
			// TODO: Perhaps we should/can never get here now?
			fileLoader = ResolveFileLoaderTarget(fileLoader);
			if (fileLoader->Exists()) {
				INFO_LOG(LOADER, "File is a PBP in a directory!");
				IdentifiedFileType ebootType = Identify_File(fileLoader, error_string);
				if (ebootType == IdentifiedFileType::PSP_ISO_NP) {
					InitMemoryForGameISO(fileLoader);
					pspFileSystem.SetStartingDirectory("disc0:/PSP_GAME/USRDIR");
					return Load_PSP_ISO(fileLoader, error_string);
				}
				else if (ebootType == IdentifiedFileType::PSP_PS1_PBP) {
					*error_string = "PS1 EBOOTs are not supported by PPSSPP.";
					coreState = CORE_BOOT_ERROR;
					return false;
				} else if (ebootType == IdentifiedFileType::ERROR_IDENTIFYING) {
					// IdentifyFile will have written to errorString.
					coreState = CORE_BOOT_ERROR;
					return false;
				}

				std::string dir = fileLoader->GetPath().GetDirectory();
				size_t pos = dir.find("PSP/GAME/");
				if (pos != std::string::npos) {
					dir = ResolvePBPDirectory(Path(dir)).ToString();
					pspFileSystem.SetStartingDirectory("ms0:/" + dir.substr(pos));
				}
				return Load_PSP_ELF_PBP(fileLoader, error_string);
			} else {
				*error_string = "No EBOOT.PBP, misidentified game";
				coreState = CORE_BOOT_ERROR;
				return false;
			}
		}

	case IdentifiedFileType::PSP_PBP:
	case IdentifiedFileType::PSP_ELF:
		{
			INFO_LOG(LOADER, "File is an ELF or loose PBP!");
			return Load_PSP_ELF_PBP(fileLoader, error_string);
		}

	case IdentifiedFileType::PSP_ISO:
	case IdentifiedFileType::PSP_ISO_NP:
	case IdentifiedFileType::PSP_DISC_DIRECTORY:	// behaves the same as the mounting is already done by now
		pspFileSystem.SetStartingDirectory("disc0:/PSP_GAME/USRDIR");
		return Load_PSP_ISO(fileLoader, error_string);

	case IdentifiedFileType::PSP_PS1_PBP:
		*error_string = "PS1 EBOOTs are not supported by PPSSPP.";
		break;

	case IdentifiedFileType::ARCHIVE_RAR:
#ifdef WIN32
		*error_string = "RAR file detected (Require WINRAR)";
#else
		*error_string = "RAR file detected (Require UnRAR)";
#endif
		break;

	case IdentifiedFileType::ARCHIVE_ZIP:
#ifdef WIN32
		*error_string = "ZIP file detected (Require WINRAR)";
#else
		*error_string = "ZIP file detected (Require UnRAR)";
#endif
		break;

	case IdentifiedFileType::ARCHIVE_7Z:
#ifdef WIN32
		*error_string = "7z file detected (Require 7-Zip)";
#else
		*error_string = "7z file detected (Require 7-Zip)";
#endif
		break;

	case IdentifiedFileType::ISO_MODE2:
		*error_string = "PSX game image detected.";
		break;

	case IdentifiedFileType::NORMAL_DIRECTORY:
		ERROR_LOG(LOADER, "Just a directory.");
		*error_string = "Just a directory.";
		break;

	case IdentifiedFileType::PPSSPP_SAVESTATE:
		*error_string = "This is a saved state, not a game.";  // Actually, we could make it load it...
		break;

	case IdentifiedFileType::PSP_SAVEDATA_DIRECTORY:
		*error_string = "This is save data, not a game."; // Actually, we could make it load it...
		break;

	case IdentifiedFileType::PPSSPP_GE_DUMP:
		return Load_PSP_GE_Dump(fileLoader, error_string);

	case IdentifiedFileType::UNKNOWN_BIN:
	case IdentifiedFileType::UNKNOWN_ELF:
	case IdentifiedFileType::UNKNOWN:
		ERROR_LOG(LOADER, "Unknown file type: %s (%s)", fileLoader->GetPath().c_str(), error_string->c_str());
		*error_string = "Unknown file type: " + fileLoader->GetPath().ToString();
		break;

	case IdentifiedFileType::ERROR_IDENTIFYING:
		*error_string = *error_string + ": " + (fileLoader ? fileLoader->LatestError() : "");
		if (error_string->empty())
			*error_string = "Error reading file";
		ERROR_LOG(LOADER, "Error while identifying file: %s", error_string->c_str());
		break;

	default:
		*error_string = StringFromFormat("Unhandled identified file type %d", (int)type);
		ERROR_LOG(LOADER, "%s", error_string->c_str());
		break;
	}

	coreState = CORE_BOOT_ERROR;
	return false;
}

bool UmdReplace(const Path &filepath, std::string &error) {
	IFileSystem *currentUMD = pspFileSystem.GetSystem("disc0:");

	if (!currentUMD) {
		error = "has no disc";
		return false;
	}

	FileLoader *loadedFile = ConstructFileLoader(filepath);

	if (!loadedFile->Exists()) {
		delete loadedFile;
		error = loadedFile->GetPath().ToVisualString() + " doesn't exist";
		return false;
	}
	UpdateLoadedFile(loadedFile);

	loadedFile = ResolveFileLoaderTarget(loadedFile);


	std::string errorString;
	IdentifiedFileType type = Identify_File(loadedFile, &errorString);

	switch (type) {
	case IdentifiedFileType::PSP_ISO:
	case IdentifiedFileType::PSP_ISO_NP:
	case IdentifiedFileType::PSP_DISC_DIRECTORY:
		if (!ReInitMemoryForGameISO(loadedFile)) {
			error = "reinit memory failed";
			return false;
		}

		break;
	default:
		error = "Unsupported file type: " + std::to_string((int)type) + " " + errorString;
		return false;
		break;
	}
	return true;
}
