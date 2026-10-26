// The updater's device shell: native Windows presentation over the portable client worker.
// Protocol, rows and message wording: docs/update/updater-re.md, sections 1, 3, 7, 8 and 12.
#include "client.h"
#include "platform.h"

#include <base/io/os_path.h>
#include <formats/update/apply.h>

#include <ctime>
#include <fstream>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
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

using updater::VersionMark;
using updater::version_mark;

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

// An expansion's box, its nested run's: the result line of its outcome over its own folder.
// [orig: Cupdate2Dlg_StateTick @ 0x408170 (UPDATE.EXE): a nested run's report, US_DONE's @0x408aac
//  when it succeeded, US_ERROR's @0x408b64 when it failed]
std::string expansion_text(const updater::Result::Expansion &expansion) {
	return result_text(expansion.ok ? kSucceeded : expansion.error, expansion.ok, expansion.dir, expansion.start);
}

bool execute(const updater::Options &options, updater::Result &result, std::string &error,
		const updater::ProgressSink &sink, const std::atomic_bool *cancel) {
	if (!options.check && sink) {
		updater::Progress waiting;
		waiting.status = "Waiting for the game to exit...";
		sink(waiting);
	}
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
		if (log) { log << p.status << " " << p.file << " " << p.done << "/" << p.total << '\n'; log.flush(); }
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
// UPDATE.EXE's dialog, IDD 102, as its resource holds it: the template's style
// (WS_POPUP | WS_VISIBLE | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SHELLFONT), WS_EX_APPWINDOW,
// 220 x 156 dialog units in MS Shell Dlg 8, and its controls by id, place and style, in the
// resource's order: the counter lines, the bar (bordered, not smooth), the elapsed line, the
// status line, the current file and the product line, every line right-aligned but the product's.
// Written here from those values; the title is ours (D-UPD-11), and the resource's 78-unit
// bitmap at the left, NovaLogic's art, is not drawn, its column left empty (D-UPD-11). There is
// no Cancel button: the window's close ends the dialog. [orig: RT_DIALOG 102 of UPDATE.EXE, read
// 2026-10-26 (updater-re section 13); the control members by Cupdate2Dlg's DDX map @0x406080]
enum : int {
	kIdDownloaded = 1000,
	kIdThroughput = 1001,
	kIdCurrentFile = 1002,
	kIdProgress = 1003,
	kIdElapsed = 1004,
	kIdStatus = 1005,
	kIdEstimated = 1006,
	kIdProduct = 1007,
};

struct TemplateControl {
	int id;
	bool progress; // msctls_progress32, else STATIC
	short x, y, cx, cy;
	DWORD style;
};
constexpr DWORD kLine = 0x50020202; // WS_CHILD | WS_VISIBLE | WS_GROUP | SS_CENTERIMAGE | SS_RIGHT
constexpr TemplateControl kControls[] = {
	{kIdDownloaded, false, 84, 112, 125, 8, kLine},
	{kIdThroughput, false, 84, 104, 125, 8, kLine},
	{kIdProgress, true, 84, 132, 126, 12, 0x50800000}, // WS_CHILD | WS_VISIBLE | WS_BORDER
	{kIdElapsed, false, 84, 76, 125, 8, kLine},
	{kIdStatus, false, 84, 33, 131, 34, 0x50020002}, // WS_CHILD | WS_VISIBLE | WS_GROUP | SS_RIGHT
	{kIdEstimated, false, 84, 89, 125, 8, kLine},
	{kIdCurrentFile, false, 84, 120, 125, 8, kLine},
	{kIdProduct, false, 84, 6, 131, 18, 0x50020000}, // WS_CHILD | WS_VISIBLE | WS_GROUP
};

// The DLGTEMPLATEEX of those values, DWORD-aligned as DialogBoxIndirectParam takes one.
std::vector<DWORD> dialog_template() {
	std::vector<WORD> words;
	const auto dword = [&words](DWORD value) { words.push_back(LOWORD(value)); words.push_back(HIWORD(value)); };
	const auto text = [&words](const wchar_t *value) { do words.push_back(WORD(*value)); while (*value++); };
	words.push_back(1); // dlgVer
	words.push_back(0xFFFF); // signature
	dword(0); // helpID
	dword(WS_EX_APPWINDOW);
	dword(WS_POPUP | WS_VISIBLE | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT | DS_FIXEDSYS);
	words.push_back(WORD(std::size(kControls)));
	for (const short value : {short(0), short(0), short(220), short(156)}) words.push_back(WORD(value));
	words.push_back(0); // no menu
	words.push_back(0); // the dialog class
	text(L"OpenNova Update");
	words.push_back(8); // points
	words.push_back(0); // weight
	words.push_back(WORD(DEFAULT_CHARSET << 8)); // italic 0, charset 1
	text(L"MS Shell Dlg");
	for (const TemplateControl &control : kControls) {
		if (words.size() % 2) words.push_back(0);
		dword(0); // helpID
		dword(0); // exStyle
		dword(control.style);
		for (const short value : {control.x, control.y, control.cx, control.cy}) words.push_back(WORD(value));
		dword(DWORD(control.id));
		if (control.progress) {
			text(PROGRESS_CLASSW);
		} else {
			words.push_back(0xFFFF);
			words.push_back(0x0082); // STATIC
		}
		words.push_back(0); // no text
		words.push_back(0); // no creation data
	}
	std::vector<DWORD> aligned((words.size() + 1) / 2, 0);
	std::memcpy(aligned.data(), words.data(), words.size() * sizeof(WORD));
	return aligned;
}

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
	bool finished = false; // the run's end reached and reported, not the dialog closed
	std::thread worker;
	// What each line shows now, by control id less 1000, and the bar's, written only on a change.
	std::array<std::string, 8> lines;
	uint32_t range = 0, position = 0;
};

void set_line(HWND window, Dialog &dialog, int id, const std::string &text) {
	std::string &shown = dialog.lines[size_t(id - kIdDownloaded)];
	if (shown == text) return;
	shown = text;
	SetDlgItemTextW(window, id, updater::wide(text).c_str());
}

// The dialog as the run has written it. The elapsed line stops at the run's end, as DONE stops
// it; before a product's run starts (our wait for the game) only the status line is written.
// [orig: Cupdate2Dlg_StateTick @ 0x408170 (UPDATE.EXE), the elapsed line written while the state
// is not DONE @0x408d8b..0x408dd6]
void draw(HWND window, Dialog &dialog, bool running) {
	updater::Progress progress;
	{ std::lock_guard<std::mutex> lock(dialog.mutex); progress = dialog.progress; }
	set_line(window, dialog, kIdStatus, progress.status);
	if (progress.start == std::chrono::steady_clock::time_point{}) return;
	if (progress.product != dialog.lines[size_t(kIdProduct - kIdDownloaded)]) {
		set_line(window, dialog, kIdProduct, progress.product);
		// The command file read restores the window. [orig: Cupdate2Dlg_ReadCommandFile @ 0x4076e0
		// (UPDATE.EXE), ShowWindow 9 @0x40779d]
		if (!progress.product.empty()) ShowWindow(window, SW_RESTORE);
	}
	set_line(window, dialog, kIdCurrentFile, progress.file);
	const auto elapsed = std::chrono::steady_clock::now() - progress.start;
	const updater::DialogLines lines = updater::dialog_lines(progress,
			uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()));
	set_line(window, dialog, kIdDownloaded, lines.downloaded);
	set_line(window, dialog, kIdThroughput, lines.throughput);
	set_line(window, dialog, kIdEstimated, lines.estimated);
	if (running) set_line(window, dialog, kIdElapsed, lines.elapsed);
	// The bar's range as retail sets it, PBM_SETRANGE's 16-bit top. [orig: Cupdate2Dlg_PrecheckCommands
	// @ 0x407820 (UPDATE.EXE) @0x407890]
	HWND bar = GetDlgItem(window, kIdProgress);
	if (progress.range != dialog.range) {
		dialog.range = progress.range;
		SendMessageW(bar, PBM_SETRANGE, 0, MAKELPARAM(0, LOWORD(progress.range)));
	}
	if (progress.position != dialog.position) {
		dialog.position = progress.position;
		SendMessageW(bar, PBM_SETPOS, WPARAM(progress.position), 0);
	}
}

// MFC's placement of a dialog whose template sets none, unowned: centred on the work area of its
// window's monitor (the primary's when it is on none), kept inside it.
// [orig: _AfxPostInitDialog @ 0x411975 (UPDATE.EXE), CDialog::CheckAutoCenter @ 0x40e8f8 (x and y
// 0, no DS_CENTER); CWnd::CenterWindow @ 0x410db2 (UPDATE.EXE), the main window's monitor
// @0x410e66]
void center(HWND window) {
	RECT rect{};
	GetWindowRect(window, &rect);
	MONITORINFO monitor{sizeof(monitor)};
	GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &monitor);
	const RECT &area = monitor.rcWork;
	const LONG width = rect.right - rect.left, height = rect.bottom - rect.top;
	LONG left = (area.right + area.left) / 2 - width / 2;
	LONG top = (area.bottom + area.top) / 2 - height / 2;
	if (left < area.left) left = area.left;
	else if (left + width > area.right) left = area.right - width;
	if (top < area.top) top = area.top;
	else if (top + height > area.bottom) top = area.bottom - height;
	SetWindowPos(window, nullptr, left, top, -1, -1, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

INT_PTR CALLBACK dialog_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
	Dialog *dialog = reinterpret_cast<Dialog *>(GetWindowLongPtrW(window, DWLP_USER));
	if (message == WM_INITDIALOG) {
		dialog = reinterpret_cast<Dialog *>(lparam);
		SetWindowLongPtrW(window, DWLP_USER, lparam);
		center(window);
		SetTimer(window, 1, USER_TIMER_MINIMUM, nullptr);
		dialog->worker = std::thread([dialog]() {
			const auto progress = [dialog](const updater::Progress &p) {
				std::lock_guard<std::mutex> lock(dialog->mutex); dialog->progress = p;
			};
			dialog->ok = execute(dialog->options, dialog->result, dialog->error, progress, &dialog->cancel);
			dialog->done.store(true);
		});
		return TRUE;
	}
	if (!dialog) return FALSE;
	if (message == WM_TIMER) {
		const bool finished = dialog->done.load();
		draw(window, *dialog, !finished);
		if (!finished) return TRUE;
		KillTimer(window, 1);
		dialog->worker.join();
		// Retail's DONE: each expansion's own box (its nested run's, succeeded or failed), the
		// result box (titled "OpenNova Update", D-UPD-11), then the launch. A revision hands over
		// to revupdat with no box. [orig: Cupdate2Dlg_StateTick @ 0x408170 (UPDATE.EXE),
		// US_DONE: Cupdate2Dlg_RunExpansionUpdates @0x408a9e ahead of the report @0x408aac]
		if (!dialog->ok || !dialog->result.revision_pending) {
			for (const updater::Result::Expansion &expansion : dialog->result.expansions)
				MessageBoxW(window, updater::wide(expansion_text(expansion)).c_str(), L"OpenNova Update",
						updater::kResultBoxType);
			const std::string text = result_text(dialog->ok ? kSucceeded : dialog->error, dialog->ok,
					dialog->options.resource_dir, dialog->start);
			MessageBoxW(window, updater::wide(text).c_str(), L"OpenNova Update", updater::kResultBoxType);
		}
		if (dialog->ok && !finish(dialog->options, dialog->args, dialog->result, dialog->error)) {
			dialog->ok = false;
			MessageBoxW(window, updater::wide(dialog->error).c_str(), L"OpenNova Update", MB_OK | MB_ICONERROR);
		}
		dialog->finished = true;
		EndDialog(window, IDOK);
		return TRUE;
	}
	// The window's close stops the run, and the dialog's default turns it into IDCANCEL. IDCANCEL
	// (the close, Esc) and IDOK (Enter) end the dialog at once, with no box and no launch, as
	// CDialog's OnCancel and OnOK end it; ours waits for the worker to stop at its next
	// interruption point, where retail's process ends with its thread. [orig: Cupdate2Dlg_StopWorker
	// @ 0x406370 (UPDATE.EXE), WM_CLOSE's handler: the worker's flag cleared, then the default;
	// Cupdate2Dlg's vftable @0x434424, slots 84 and 85: CDialog::OnOK @ 0x40e597 and
	// CDialog::OnCancel @ 0x40e5b0 (UPDATE.EXE), not overridden]
	if (message == WM_CLOSE) {
		dialog->cancel.store(true);
		return FALSE;
	}
	if (message == WM_COMMAND && (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL)) {
		dialog->cancel.store(true);
		KillTimer(window, 1);
		EndDialog(window, LOWORD(wparam));
		return TRUE;
	}
	return FALSE;
}

int show_dialog(const updater::Options &options, const VersionMark &start, const std::vector<std::string> &args) {
	INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS};
	InitCommonControlsEx(&controls);
	Dialog dialog;
	dialog.options = options;
	dialog.start = start;
	dialog.args = args;
	const std::vector<DWORD> resource = dialog_template();
	DialogBoxIndirectParamW(GetModuleHandleW(nullptr), reinterpret_cast<LPCDLGTEMPLATEW>(resource.data()),
			nullptr, dialog_proc, reinterpret_cast<LPARAM>(&dialog));
	if (dialog.worker.joinable()) dialog.worker.join();
	return dialog.finished && dialog.ok ? 0 : 1;
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
	for (const updater::Result::Expansion &expansion : result.expansions)
		updater::write_stdout((expansion.ok ? "" : "ERROR ") + expansion_text(expansion) + "\n");
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
