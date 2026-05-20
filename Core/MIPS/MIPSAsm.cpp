#include <cctype>
#include <cstdarg>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "ext/armips/Core/Assembler.h"
#include "ext/armips/Core/FileManager.h"
#include "Core/Debugger/SymbolMap.h"
#include "Core/MemMapHelpers.h"
#include "Core/MIPS/MIPSAsm.h"

class PspAssemblerFile : public AssemblerFile {
public:
	PspAssemblerFile() {
		address = 0;
	}

	bool open(bool onlyCheck) override{ return true; };
	void close() override { };
	bool isOpen() override { return true; };
	bool write(void *data, size_t length) override {
		if (!Memory::IsValidAddress((u32)(address+length-1)))
			return false;

		Memory::Memcpy((u32)address, data, (u32)length, "Debugger");
		
		// In case this is a delay slot or combined instruction, clear cache above it too.
		mipsr4k.InvalidateICache((u32)(address - 4), (int)length + 4);

		address += length;
		return true;
	}
	int64_t getVirtualAddress() override { return address; };
	int64_t getPhysicalAddress() override { return getVirtualAddress(); };
	int64_t getHeaderSize() override { return 0; }
	bool seekVirtual(int64_t virtualAddress) override {
		if (!Memory::IsValidAddress(virtualAddress))
			return false;
		address = virtualAddress;
		return true;
	}
	bool seekPhysical(int64_t physicalAddress) override { return seekVirtual(physicalAddress); }
	const fs::path &getFileName() override { return dummyFilename_; }
private:
	u64 address;
	fs::path dummyFilename_;
};

bool MipsAssembleOpcode(std::string_view line, DebugInterface *cpu, u32 address, std::string *error) {
	std::vector<std::string> errors;

	char str[64];
	snprintf(str, 64, ".psp\n.org 0x%08X\n", address);

	ArmipsArguments args;
	args.mode = ArmipsMode::MEMORY;
	args.content = str + std::string(line);
	args.silent = true;
	args.memoryFile.reset(new PspAssemblerFile());
	args.errorsResult = &errors;

	if (g_symbolMap) {
		g_symbolMap->GetLabels(args.labels);
	}

	// Auto-resolve `pos_0xXXXXXXXX` tokens as literal addresses. The disasm
	// "Copy as CWcheat" / branch renderer emits `pos_0xAABBCCDD` for targets
	// that don't have a real label; this lets the assembler accept that text
	// without choking on a missing symbol. If a real label by that same name
	// already exists (added by the user), it wins — we only inject for names
	// not already defined.
	{
		std::set<std::string> existing;
		for (const auto &l : args.labels)
			existing.insert(l.name.string());

		const std::string s(line);
		size_t scan = 0;
		while (scan < s.size()) {
			size_t hit = s.find("pos_0x", scan);
			if (hit == std::string::npos)
				break;
			bool at_boundary = (hit == 0) ||
				!(isalnum((unsigned char)s[hit - 1]) || s[hit - 1] == '_');
			size_t hex_start = hit + 6;  // strlen("pos_0x")
			size_t hex_end = hex_start;
			while (hex_end < s.size() && isxdigit((unsigned char)s[hex_end]))
				++hex_end;
			if (at_boundary && hex_end > hex_start) {
				std::string name = s.substr(hit, hex_end - hit);
				if (existing.find(name) == existing.end()) {
					uint64_t value = 0;
					for (size_t i = hex_start; i < hex_end; ++i) {
						value <<= 4;
						char c = s[i];
						if (c >= '0' && c <= '9') value |= (c - '0');
						else if (c >= 'a' && c <= 'f') value |= (c - 'a' + 10);
						else value |= (c - 'A' + 10);
					}
					LabelDefinition def;
					def.name = Identifier(name);
					def.value = (int64_t)value;
					args.labels.push_back(def);
					existing.insert(name);
				}
				scan = hex_end;
			} else {
				scan = hit + 1;
			}
		}
	}

	error->clear();
	if (!runArmips(args)) {
		for (size_t i = 0; i < errors.size(); i++) {
			(*error) += errors[i];
			if (i != errors.size() - 1)
				error->push_back('\n');
		}

		return false;
	}

	return true;
}
