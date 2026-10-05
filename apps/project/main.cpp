// opennova-project: the OpenNova Editor's project session on the command line
// (ADR 0046 d4, S13 A7). See cli_verbs.h for the verbs and exit codes.
#include "cli_verbs.h"

#include <cstdio>

#ifdef _WIN32
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

int main(int argc, char **argv) {
#ifdef _WIN32
	// The arguments as UTF-8, the paths every verb takes (editor/project/project_files.h): the
	// narrow argv is the ANSI code page's, which spells a folder named outside it as another or
	// as '?'. Read wide from the command line and turned to UTF-8.
	int count = 0;
	wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), &count);
	if (wide != nullptr && count >= 1) {
		std::vector<std::string> args;
		for (int i = 0; i < count; ++i) {
			const int length = static_cast<int>(wcslen(wide[i]));
			const int needed = WideCharToMultiByte(CP_UTF8, 0, wide[i], length, nullptr, 0, nullptr, nullptr);
			std::string arg(static_cast<size_t>(needed > 0 ? needed : 0), '\0');
			if (needed > 0) WideCharToMultiByte(CP_UTF8, 0, wide[i], length, arg.data(), needed, nullptr, nullptr);
			args.push_back(std::move(arg));
		}
		LocalFree(wide);
		std::vector<const char *> pointers;
		for (const std::string &arg : args) pointers.push_back(arg.c_str());
		return opennova::project::run_project_command(count - 1, pointers.data() + 1, stdout, stderr);
	}
	if (wide != nullptr) LocalFree(wide);
#endif
	return opennova::project::run_project_command(argc - 1, argv + 1, stdout, stderr);
}
