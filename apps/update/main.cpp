// The updater's device shell: native Windows presentation over the portable client worker.
// Protocol, rows and message wording: docs/update/updater-re.md, sections 1, 3, 7, 8 and 12.
#include "client.h"
#include "platform.h"

#include <base/io/file_io.h>
#include <base/io/os_path.h>
#include <base/vfs/vfs.h>
#include <formats/update/apply.h>
#include <formats/update/command_file.h>
#include <formats/update/update_crc.h>
#include <formats/update/update_wizard.h>

#include <ctime>
#include <fstream>

#include <atomic>
#include <mutex>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#endif

namespace {
using namespace opennova;

constexpr const char *kSucceeded = "Operation completed successfully!";

// The installed command file of the folder's game (its wizard's CMD), as the result box reads it: its
// CRC and its VERSION, or none when it does not open. Retail reads version.txt here; ours reads the
// command file the release wrote, whose VERSION is the release's (D-UPD-11).
struct VersionMark {
	bool present = false;
	uint32_t crc = 0;
	std::string version;
};

VersionMark version_mark(const std::filesystem::path &dir) {
	// The wizard's CMD; a folder with none is OpenNova's own (the client's rule, discover_products).
	std::string wizard;
	const std::string command = vfs_update_wizard_text(io::utf8_path(dir), wizard)
	                                    ? update::read_update_wizard(wizard).cmd
	                                    : std::string(update::kAcidGambitCommandFile);
	std::vector<uint8_t> bytes;
	std::string ignored;
	if (command.empty() || !update::is_safe_row_name(command) ||
			!io::read_file_bytes(io::utf8_path(dir / command), bytes, ignored))
		return {};
	VersionMark mark{true, update::command_file_crc(bytes.data(), bytes.size()), std::string()};
	if (const std::optional<update::CommandFile> file =
					update::read_command_file(std::string(bytes.begin(), bytes.end())))
		mark.version = file->version;
	return mark;
}

// The result box's text as retail composes it: the result line, then "No Update Needed!" when a
// successful run left the command file as it found it, else "Current Version:" and its VERSION; nothing
// more when it does not open. [orig: Cupdate2Dlg_ReportResult @ 0x405140 (UPDATE.EXE), which reads
// version.txt in its place: D-UPD-11]
std::string result_text(const std::string &line, bool ok, const std::filesystem::path &dir,
		const VersionMark &start) {
	const VersionMark now = version_mark(dir);
	if (!now.present) return line;
	if (ok && start.present && now.crc == start.crc) return line + "\r\nNo Update Needed!\r\n";
	return now.version.empty() ? line : line + "\r\nCurrent Version:\r\n" + now.version + "\r\n";
}

bool execute(const updater::Options &options, updater::Result &result, std::string &error,
		const updater::ProgressSink &sink, const std::atomic_bool *cancel) {
	if (!options.check && sink) sink({"Waiting for the game to exit...", "", 0, 0});
	if (!options.check && !updater::wait_for_parent(options.parent_pid, options.parent_window, error, cancel))
		return false;
	std::vector<updater::Product> products;
	std::ofstream log;
	if (options.verbose && !options.check) {
		// update.log truncated in the install's directory, headed by the time.
		// [orig: UpdateLog_Open @ 0x4040b0 (UPDATE.EXE)]
		log.open(options.resource_dir / "update.log", std::ios::trunc);
		const std::time_t now = std::time(nullptr);
		if (log) log << "Update log : " << std::asctime(std::localtime(&now)) << '\n';
	}
	const auto progress = [&](const updater::Progress &p) {
		if (log) { log << p.status << " " << p.file << " " << p.bytes << "/" << p.total << '\n'; log.flush(); }
		if (sink) sink(p);
	};
	return updater::discover_products(options, products, error) &&
			updater::run(options, products, result, error, progress, cancel);
}

// After a run: revupdat replaces a staged update.new and relaunches the updater (retail hands
// over after every loose pass, D-UPD-4), else /RUN starts the command file's EXE (legacy) or
// opennova.exe. [orig: Cupdate2Dlg_StateTick @ 0x408170 (UPDATE.EXE), US_RUN_REVUPDAT and US_DONE]
bool finish(const updater::Options &options, const std::vector<std::string> &original,
		const updater::Result &result, std::string &error) {
	if (!result.revision_pending && !options.run) return true;
	std::error_code ec;
	const auto launch_dir = std::filesystem::current_path(ec);
	if (ec) { error = "Cannot determine the launch directory."; return false; }
	if (result.revision_pending) {
		return updater::launch_detached(options.runtime_dir / "revupdat.exe", launch_dir,
				updater::revision_handoff_arguments(options, original, updater::process_id()), error);
	}
	std::filesystem::path program = options.runtime_dir / "opennova.exe";
	if (options.legacy && !result.launch_exe.empty()) {
		if (!update::is_safe_row_name(result.launch_exe)) { error = "Unsafe EXE in update command."; return false; }
		program = options.resource_dir / io::os_path(result.launch_exe);
	}
	auto run_args = options.run_args;
	if (!options.legacy && run_args.empty())
		run_args = {"--", "/game", options.game, "--resource-dir", io::utf8_path(options.resource_dir)};
	return updater::launch_detached(program, launch_dir, run_args, error);
}

#ifdef _WIN32
struct Dialog {
	updater::Options options;
	VersionMark start;
	updater::Result result;
	std::vector<std::string> args;
	std::atomic_bool cancel{false};
	std::atomic_bool done{false};
	std::mutex mutex;
	updater::Progress progress;
	std::string error;
	bool ok = false;
	HWND status = nullptr, file = nullptr, bar = nullptr, button = nullptr;
	std::thread worker;
};

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
	Dialog *dialog = reinterpret_cast<Dialog *>(GetWindowLongPtrW(window, GWLP_USERDATA));
	if (message == WM_CREATE) {
		dialog = static_cast<Dialog *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
		SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
		const auto control = [&](const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h) {
			HWND result = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h,
					window, nullptr, GetModuleHandleW(nullptr), nullptr);
			SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
			return result;
		};
		dialog->status = control(L"STATIC", L"Checking for update...", 0, 22, 24, 540, 36);
		dialog->file = control(L"STATIC", L"", SS_PATHELLIPSIS, 22, 67, 540, 34);
		dialog->bar = control(PROGRESS_CLASSW, L"", PBS_SMOOTH, 22, 110, 540, 22);
		SendMessageW(dialog->bar, PBM_SETRANGE32, 0, 1000);
		dialog->button = control(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 462, 151, 100, 30);
		SetTimer(window, 1, 100, nullptr);
		dialog->worker = std::thread([dialog]() {
			const auto progress = [dialog](const updater::Progress &p) {
				std::lock_guard<std::mutex> lock(dialog->mutex); dialog->progress = p;
			};
			dialog->ok = execute(dialog->options, dialog->result, dialog->error, progress, &dialog->cancel);
			dialog->done.store(true);
		});
		return 0;
	}
	if (!dialog) return DefWindowProcW(window, message, wparam, lparam);
	if (message == WM_TIMER) {
		updater::Progress progress;
		{ std::lock_guard<std::mutex> lock(dialog->mutex); progress = dialog->progress; }
		SetWindowTextW(dialog->status, updater::wide(progress.status).c_str());
		std::string file = progress.file;
		if (progress.total) file += "  " + std::to_string(progress.bytes / 1024) + "/" + std::to_string(progress.total / 1024) + " KBytes";
		SetWindowTextW(dialog->file, updater::wide(file).c_str());
		SendMessageW(dialog->bar, PBM_SETPOS, progress.total ? WPARAM(progress.bytes * 1000 / progress.total) : 0, 0);
		if (dialog->done.load()) {
			KillTimer(window, 1);
			dialog->worker.join();
			// Retail's DONE: each failed expansion's own box (its nested run's), the result box (titled
			// "OpenNova Update", D-UPD-11), then the launch. A revision hands over to revupdat with
			// no box.
			if (!dialog->ok || !dialog->result.revision_pending) {
				for (const updater::Result::Failure &failure : dialog->result.expansion_failures)
					MessageBoxW(window, updater::wide(result_text(failure.error, false, failure.dir, {})).c_str(),
							L"OpenNova Update", MB_OK | MB_ICONERROR);
				const std::string text = result_text(dialog->ok ? kSucceeded : dialog->error, dialog->ok,
						dialog->options.resource_dir, dialog->start);
				MessageBoxW(window, updater::wide(text).c_str(), L"OpenNova Update",
						MB_OK | (dialog->ok ? MB_ICONINFORMATION : MB_ICONERROR));
			}
			if (dialog->ok && !finish(dialog->options, dialog->args, dialog->result, dialog->error)) {
				dialog->ok = false;
				MessageBoxW(window, updater::wide(dialog->error).c_str(), L"OpenNova Update", MB_OK | MB_ICONERROR);
			}
			DestroyWindow(window);
		}
		return 0;
	}
	if (message == WM_CLOSE || (message == WM_COMMAND && reinterpret_cast<HWND>(lparam) == dialog->button)) {
		dialog->cancel.store(true);
		EnableWindow(dialog->button, FALSE);
		return 0;
	}
	if (message == WM_DESTROY) { PostQuitMessage(dialog->ok ? 0 : 1); return 0; }
	return DefWindowProcW(window, message, wparam, lparam);
}

int show_dialog(const updater::Options &options, const VersionMark &start, const std::vector<std::string> &args) {
	INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS};
	InitCommonControlsEx(&controls);
	WNDCLASSW cls{};
	cls.lpfnWndProc = window_proc;
	cls.hInstance = GetModuleHandleW(nullptr);
	cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
	cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
	cls.lpszClassName = L"OpenNovaUpdate";
	RegisterClassW(&cls);
	Dialog dialog;
	dialog.options = options;
	dialog.start = start;
	dialog.args = args;
	HWND window = CreateWindowExW(WS_EX_APPWINDOW, cls.lpszClassName, L"OpenNova Update",
			WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
			600, 240, nullptr, nullptr, cls.hInstance, &dialog);
	if (!window) return 1;
	ShowWindow(window, SW_SHOW);
	MSG message{};
	while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
	if (dialog.worker.joinable()) { dialog.cancel.store(true); dialog.worker.join(); }
	return dialog.ok ? 0 : 1;
}
#endif

int app_main(const std::vector<std::string> &args, const std::filesystem::path &executable) {
	updater::Options options;
	std::string error;
	if (!updater::parse_options(args, executable, options, error)) {
		updater::write_stdout(error + "\n" + updater::usage());
		return 2;
	}
	if (options.help) { updater::write_stdout(updater::usage()); return 0; }
	if (options.check) {
		updater::Result result;
		if (!execute(options, result, error, {}, nullptr)) {
			updater::write_stdout("ERROR " + error + "\n"); return 1;
		}
		updater::write_stdout(result.available ? "UPDATE AVAILABLE\n" : "CURRENT\n");
		return result.available ? 10 : 0;
	}
	const VersionMark start = version_mark(options.resource_dir);
#ifdef _WIN32
	return show_dialog(options, start, args);
#else
	updater::Result result;
	if (!execute(options, result, error, [](const updater::Progress &p) {
			updater::write_stdout(p.status + " " + p.file + "\n");
		}, nullptr)) {
		updater::write_stdout("ERROR " + result_text(error, false, options.resource_dir, start) + "\n");
		return 1;
	}
	for (const updater::Result::Failure &failure : result.expansion_failures)
		updater::write_stdout("ERROR " + result_text(failure.error, false, failure.dir, {}) + "\n");
	if (!result.revision_pending)
		updater::write_stdout(result_text(kSucceeded, true, options.resource_dir, start) + "\n");
	if (!finish(options, args, result, error)) {
		updater::write_stdout("ERROR " + error + "\n");
		return 1;
	}
	return 0;
#endif
}
} // namespace

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
	return app_main(opennova::updater::process_arguments(), opennova::updater::executable_path());
}
#else
int main(int argc, char **argv) {
	return app_main(opennova::updater::process_arguments(argc, argv), opennova::updater::executable_path(argv[0]));
}
#endif
