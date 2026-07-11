#include <opennova/retail_hook/windows/validation_window.h>
#include <opennova/retail_hook/windows/retail_capture_agent.h>

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace opennova::retail_hook::windows {
namespace {

static_assert(sizeof(void*) == 4, "The retail hook must be compiled for 32-bit Windows.");

constexpr wchar_t kWindowClass[] = L"OpenNovaRetailValidationWindow";
constexpr wchar_t kReadOnlyWindowTitle[] =
    L"OpenNova Retail Validation (read-only)";
constexpr wchar_t kWriteEnabledWindowTitle[] =
    L"OpenNova Retail Validation (WRITE-ENABLED)";
constexpr UINT_PTR kSampleTimer = 1;
constexpr UINT kSampleIntervalMs = 750;
constexpr int kLineHeight = 19;

[[nodiscard]] std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return L"<text too long>";
    }

    const int source_size = static_cast<int>(text.size());
    int output_size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), source_size, nullptr, 0);
    UINT code_page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (output_size == 0) {
        code_page = CP_ACP;
        flags = 0;
        output_size = MultiByteToWideChar(
            code_page, flags, text.data(), source_size, nullptr, 0);
    }
    if (output_size <= 0) {
        return L"<invalid text>";
    }

    std::wstring result(static_cast<std::size_t>(output_size), L'\0');
    if (MultiByteToWideChar(
            code_page,
            flags,
            text.data(),
            source_size,
            result.data(),
            output_size) != output_size) {
        return L"<invalid text>";
    }
    return result;
}

class ValidationWindow final {
public:
    ValidationWindow(
        HINSTANCE module,
        ValidationSession* session,
        RetailCaptureAgent* capture,
        bool writes_enabled,
        std::wstring window_identity,
        std::wstring startup_diagnostic)
        : module_(module),
          session_(session),
          capture_(capture),
          writes_enabled_(writes_enabled),
          window_identity_(std::move(window_identity)),
          startup_diagnostic_(std::move(startup_diagnostic)) {}

    [[nodiscard]] DWORD run() {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = &ValidationWindow::window_proc;
        window_class.hInstance = module_;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.lpszClassName = kWindowClass;
        if (RegisterClassExW(&window_class) == 0 &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return GetLastError();
        }

        std::wstring window_title = writes_enabled_
            ? kWriteEnabledWindowTitle
            : kReadOnlyWindowTitle;
        if (!window_identity_.empty()) {
            window_title += L" - ";
            window_title += window_identity_;
        }
        HWND window = CreateWindowExW(
            WS_EX_APPWINDOW,
            kWindowClass,
            window_title.c_str(),
            WS_OVERLAPPEDWINDOW | WS_VSCROLL,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            920,
            640,
            nullptr,
            nullptr,
            module_,
            this);
        if (window == nullptr) {
            return GetLastError();
        }

        sample();
        ShowWindow(window, SW_SHOWNORMAL);
        UpdateWindow(window);
        if (SetTimer(window, kSampleTimer, kSampleIntervalMs, nullptr) == 0) {
            DestroyWindow(window);
            return GetLastError();
        }

        MSG message{};
        BOOL result = FALSE;
        while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return result < 0 ? GetLastError() : static_cast<DWORD>(message.wParam);
    }

private:
    static LRESULT CALLBACK window_proc(
        HWND window,
        UINT message,
        WPARAM wparam,
        LPARAM lparam) {
        ValidationWindow* self = reinterpret_cast<ValidationWindow*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            self = static_cast<ValidationWindow*>(create->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(
                window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self != nullptr) {
            return self->handle_message(message, wparam, lparam);
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_TIMER:
            if (wparam == kSampleTimer) {
                sample();
                InvalidateRect(window_, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) {
                DestroyWindow(window_);
                return 0;
            }
            if (wparam == VK_F5) {
                sample();
                InvalidateRect(window_, nullptr, FALSE);
                return 0;
            }
            if (wparam >= VK_F6 && wparam <= VK_F10) {
                mutate(
                    static_cast<UINT>(wparam),
                    (GetKeyState(VK_SHIFT) & 0x8000) != 0);
                sample();
                InvalidateRect(window_, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_VSCROLL:
            scroll(LOWORD(wparam));
            return 0;
        case WM_MOUSEWHEEL:
            scroll_lines(
                -(GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA) * 3);
            return 0;
        case WM_SIZE:
            update_scrollbar();
            return 0;
        case WM_PAINT:
            paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_CLOSE:
            DestroyWindow(window_);
            return 0;
        case WM_DESTROY:
            KillTimer(window_, kSampleTimer);
            PostQuitMessage(0);
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
            return DefWindowProcW(window_, message, wparam, lparam);
        default:
            break;
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }

    [[nodiscard]] static std::wstring hexadecimal(std::uint32_t value) {
        std::wostringstream output;
        output << L"0x" << std::uppercase << std::hex
               << std::setw(8) << std::setfill(L'0') << value;
        return output.str();
    }

    void mutate(UINT key, bool shifted) {
        if (!writes_enabled_ || session_ == nullptr) {
            last_mutation_ =
                L"Mutation rejected: launch without --allow-writes.";
            return;
        }
        const ValidationSnapshot snapshot =
            session_->sample(ProbeSet::players);
        if (!snapshot) {
            last_mutation_ =
                L"Mutation pre-sample failed: " +
                widen(snapshot.check.detail);
            return;
        }
        const auto local = std::find_if(
            snapshot.players.begin(),
            snapshot.players.end(),
            [](const PlayerObservation& player) {
                return player.is_local;
            });
        if (local == snapshot.players.end()) {
            last_mutation_ =
                L"Mutation rejected: no local player is present.";
            return;
        }

        Mutation request{};
        if (key == VK_F6) {
            if (local->health ==
                std::numeric_limits<std::int16_t>::min()) {
                last_mutation_ =
                    L"Mutation rejected: health is already at its type limit.";
                return;
            }
            request = SetPlayerHealth{
                local->slot,
                local->owner_connection_id,
                local->health,
                static_cast<std::int16_t>(local->health - 1),
            };
        } else if (key == VK_F7) {
            request = SetPlayerTeam{
                local->slot,
                local->owner_connection_id,
                local->team,
                static_cast<std::int16_t>(
                    local->team == 1 ? 2 : 1),
            };
        } else if (key == VK_F8) {
            request = SetEquippedAdmIndex{
                local->slot,
                local->owner_connection_id,
                local->equipped_adm_index,
                static_cast<std::uint8_t>(
                    local->equipped_adm_index + 1U),
            };
        } else {
            if (!local->weapon.has_value()) {
                last_mutation_ =
                    L"Mutation rejected: no active weapon definition.";
                return;
            }
            const WeaponObservation& weapon = *local->weapon;
            if (key == VK_F9) {
                request = SetActiveWeaponFov{
                    local->slot,
                    local->owner_connection_id,
                    weapon.address,
                    weapon.render_fov,
                    weapon.render_fov - 1.0F,
                };
            } else {
                SetActiveWeaponPose pose{};
                pose.player_slot = local->slot;
                pose.expected_owner_connection_id =
                    local->owner_connection_id;
                pose.expected_weapon_address = weapon.address;
                pose.pose = shifted
                    ? ActiveWeaponPose::aimed
                    : ActiveWeaponPose::hip;
                pose.expected_value = shifted
                    ? WeaponPoseValue{
                          weapon.alternate_position_x,
                          weapon.alternate_position_y,
                          weapon.alternate_position_z,
                          weapon.alternate_rotation_yaw_raw,
                          weapon.alternate_rotation_pitch_raw,
                          weapon.alternate_rotation_roll_raw,
                      }
                    : WeaponPoseValue{
                          weapon.position_x,
                          weapon.position_y,
                          weapon.position_z,
                          weapon.rotation_yaw_raw,
                          weapon.rotation_pitch_raw,
                          weapon.rotation_roll_raw,
                      };
                pose.value = pose.expected_value;
                pose.value.position_x += 1.0F;
                request = pose;
            }
        }

        const MutationResult result = session_->apply(request);
        const bool audit_queued =
            capture_ != nullptr &&
            capture_->capture_mutation(result);
        const bool possibly_applied =
            result.audit.stage == MutationStage::write_attempted ||
            result.audit.stage == MutationStage::written;
        if (result) {
            last_mutation_ = audit_queued
                ? L"Mutation verified and audit queued."
                : L"Mutation verified, but the bounded audit queue rejected "
                  L"the record.";
        } else if (possibly_applied) {
            last_mutation_ =
                L"WRITE MAY HAVE OCCURRED but verification failed [" +
                widen(validation_error_name(result.check.error)) +
                L"]: " + widen(result.check.detail) +
                L". Do not blindly retry." +
                (audit_queued
                     ? L" (unverified audit queued)"
                     : L" (audit queue rejected the unverified record)");
        } else {
            last_mutation_ = L"Mutation rejected before any write [" +
                widen(validation_error_name(result.check.error)) +
                L"]: " + widen(result.check.detail) +
                (audit_queued
                     ? L" (audit queued)"
                     : L" (audit queue rejected the record)");
        }
    }

    void sample() {
        lines_.clear();
        has_error_ = false;
        lines_.emplace_back(
            writes_enabled_
            ? L"OpenNova retail memory validator | WRITE-ENABLED"
            : L"OpenNova retail memory validator | READ ONLY");
        lines_.emplace_back(
            L"F5: sample | Esc: close | UI refresh: 750 ms "
            L"(parity capture: nominal 60 Hz)");
        if (writes_enabled_) {
            lines_.emplace_back(
                L"F6: health -1 | F7: toggle team | F8: ADM +1 | "
                L"F9: FOV -1 | F10: hip X +1 | Shift+F10: aimed X +1");
        }
        if (!last_mutation_.empty()) {
            lines_.push_back(last_mutation_);
        }
        if (!startup_diagnostic_.empty()) {
            lines_.push_back(startup_diagnostic_);
        }
        if (session_ == nullptr) {
            has_error_ = true;
            lines_.emplace_back(L"No validation session is available.");
            update_scrollbar();
            return;
        }

        const ValidationSnapshot snapshot = session_->sample(ProbeSet::players);
        if (!snapshot) {
            has_error_ = true;
            lines_.emplace_back(
                L"Sample failed [" +
                widen(validation_error_name(snapshot.check.error)) +
                L"]: " + widen(snapshot.check.detail));
            update_scrollbar();
            return;
        }

        lines_.emplace_back(
            L"Profile: " + widen(snapshot.profile_name) +
            L" | player pool: " +
            std::to_wstring(snapshot.player_pool_used) + L"/" +
            std::to_wstring(snapshot.player_pool_capacity));
        lines_.emplace_back(
            L"Observed players: " + std::to_wstring(snapshot.players.size()));
        for (const PoolDescriptorObservation& pool :
             snapshot.pool_descriptors) {
            std::wostringstream descriptor;
            descriptor << L"Pool " << pool.index
                       << L": data=" << hexadecimal(pool.data_address)
                       << L" stride=" << pool.element_size
                       << L" used=" << pool.used
                       << L" capacity=" << pool.capacity
                       << (pool.index == 0
                               ? L" | entity records decoded"
                               : L" | descriptor only");
            lines_.push_back(descriptor.str());
        }
        for (const PlayerObservation& player : snapshot.players) {
            append_player(player);
        }
        update_scrollbar();
    }

    void append_player(const PlayerObservation& player) {
        std::wostringstream identity;
        identity << L"[" << player.slot << L"] "
                 << (player.is_local ? L"*LOCAL* " : L"")
                 << widen(player.name)
                 << L" | entity=" << hexadecimal(player.address);
        lines_.push_back(identity.str());

        std::wostringstream identifiers;
        identifiers << L"    ids: owner=" << player.owner_connection_id
                    << L" dcb=" << player.dcb_id
                    << L" ssn=" << player.ssn
                    << L" net=" << player.net_id
                    << L" command_group=" << player.command_group;
        lines_.push_back(identifiers.str());

        std::wostringstream state;
        state << L"    hp=" << player.health
                 << L" armor=" << player.armor
                 << L" team=" << player.team
                 << L" class=" << static_cast<unsigned>(player.player_class)
                 << L" adm=" << static_cast<unsigned>(player.equipped_adm_index)
                 << L" anim=" << static_cast<unsigned>(player.animation_slot)
                 << L" flags=" << hexadecimal(player.flags);
        lines_.push_back(state.str());

        std::wostringstream transform;
        transform << L"    raw pos=("
                  << player.position_x_raw << L", "
                  << player.position_y_raw << L", "
                  << player.position_z_raw << L") camera=("
                  << player.camera_offset_x_raw << L", "
                  << player.camera_offset_y_raw << L", "
                  << player.camera_offset_z_raw << L") orient=("
                  << player.yaw_raw << L", "
                  << player.pitch_raw << L", "
                  << player.roll_raw << L") ground="
                  << hexadecimal(player.ground_entity_address);
        lines_.push_back(transform.str());

        if (player.weapon.has_value()) {
            append_weapon(*player.weapon);
        } else if (!player.weapon_check) {
            has_error_ = true;
            lines_.emplace_back(
                L"    weapon probe failed [" +
                widen(validation_error_name(player.weapon_check.error)) +
                L"]: " + widen(player.weapon_check.detail));
        } else {
            lines_.emplace_back(L"    weapon: no equipped definition");
        }
    }

    void append_weapon(const WeaponObservation& weapon) {
        std::wostringstream identity;
        identity << L"    weapon: " << widen(weapon.name)
                 << L" definition=" << hexadecimal(weapon.address)
                 << L" render_fov=" << std::fixed << std::setprecision(2)
                 << weapon.render_fov;
        lines_.push_back(identity.str());

        std::wostringstream primary;
        primary << std::fixed << std::setprecision(3)
                << L"        primary pos=("
                << weapon.position_x << L", "
                << weapon.position_y << L", "
                << weapon.position_z << L") raw BAM rot=("
                << weapon.rotation_yaw_raw << L", "
                << weapon.rotation_pitch_raw << L", "
                << weapon.rotation_roll_raw << L")";
        lines_.push_back(primary.str());

        std::wostringstream alternate;
        alternate << std::fixed << std::setprecision(3)
                  << L"        alternate pos=("
                  << weapon.alternate_position_x << L", "
                  << weapon.alternate_position_y << L", "
                  << weapon.alternate_position_z << L") raw BAM rot=("
                  << weapon.alternate_rotation_yaw_raw << L", "
                  << weapon.alternate_rotation_pitch_raw << L", "
                  << weapon.alternate_rotation_roll_raw << L")";
        lines_.push_back(alternate.str());
    }

    [[nodiscard]] int visible_line_count() const noexcept {
        if (window_ == nullptr) {
            return 1;
        }
        RECT client{};
        if (GetClientRect(window_, &client) == FALSE) {
            return 1;
        }
        return std::max(
            1, static_cast<int>((client.bottom - 12) / kLineHeight));
    }

    [[nodiscard]] int maximum_scroll_line() const noexcept {
        const std::size_t visible =
            static_cast<std::size_t>(visible_line_count());
        if (lines_.size() <= visible) {
            return 0;
        }
        const std::size_t maximum = lines_.size() - visible;
        return maximum > static_cast<std::size_t>(std::numeric_limits<int>::max())
            ? std::numeric_limits<int>::max()
            : static_cast<int>(maximum);
    }

    void update_scrollbar() {
        if (window_ == nullptr) {
            return;
        }
        scroll_line_ = std::clamp(scroll_line_, 0, maximum_scroll_line());
        const std::size_t line_max =
            std::min<std::size_t>(
                lines_.empty() ? 0 : lines_.size() - 1,
                static_cast<std::size_t>(std::numeric_limits<int>::max()));
        SCROLLINFO information{};
        information.cbSize = sizeof(information);
        information.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        information.nMin = 0;
        information.nMax = static_cast<int>(line_max);
        information.nPage = static_cast<UINT>(visible_line_count());
        information.nPos = scroll_line_;
        SetScrollInfo(window_, SB_VERT, &information, TRUE);
    }

    void scroll_lines(int delta) {
        const long long requested =
            static_cast<long long>(scroll_line_) + delta;
        scroll_line_ = static_cast<int>(std::clamp<long long>(
            requested, 0, maximum_scroll_line()));
        update_scrollbar();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void scroll(UINT command) {
        switch (command) {
        case SB_LINEUP:
            scroll_lines(-1);
            return;
        case SB_LINEDOWN:
            scroll_lines(1);
            return;
        case SB_PAGEUP:
            scroll_lines(-visible_line_count());
            return;
        case SB_PAGEDOWN:
            scroll_lines(visible_line_count());
            return;
        case SB_TOP:
            scroll_line_ = 0;
            break;
        case SB_BOTTOM:
            scroll_line_ = maximum_scroll_line();
            break;
        case SB_THUMBPOSITION:
        case SB_THUMBTRACK: {
            SCROLLINFO information{};
            information.cbSize = sizeof(information);
            information.fMask = SIF_TRACKPOS;
            if (GetScrollInfo(window_, SB_VERT, &information) != FALSE) {
                scroll_line_ = information.nTrackPos;
            }
            break;
        }
        default:
            return;
        }
        update_scrollbar();
        InvalidateRect(window_, nullptr, FALSE);
    }

    void paint() {
        PAINTSTRUCT paint_state{};
        HDC device = BeginPaint(window_, &paint_state);
        if (device == nullptr) {
            return;
        }

        RECT client{};
        GetClientRect(window_, &client);
        HBRUSH background = CreateSolidBrush(RGB(24, 26, 31));
        if (background != nullptr) {
            FillRect(device, &client, background);
            DeleteObject(background);
        }

        SetBkMode(device, TRANSPARENT);
        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HGDIOBJ previous_font = SelectObject(device, font);
        int y = 12;
        for (std::size_t index = static_cast<std::size_t>(scroll_line_);
             index < lines_.size();
             ++index) {
            if (index == 0) {
                SetTextColor(device, RGB(112, 220, 156));
            } else if (has_error_) {
                SetTextColor(device, RGB(255, 128, 128));
            } else {
                SetTextColor(device, RGB(220, 224, 232));
            }

            RECT line{12, y, std::max(12L, client.right - 12), y + kLineHeight};
            DrawTextW(
                device,
                lines_[index].c_str(),
                static_cast<int>(lines_[index].size()),
                &line,
                DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            y += kLineHeight;
            if (y >= client.bottom) {
                break;
            }
        }
        SelectObject(device, previous_font);
        EndPaint(window_, &paint_state);
    }
    HINSTANCE module_{};
    HWND window_{};
    ValidationSession* session_{};
    RetailCaptureAgent* capture_{};
    bool writes_enabled_{};
    std::wstring window_identity_;
    std::wstring startup_diagnostic_;
    std::wstring last_mutation_;
    std::vector<std::wstring> lines_;
    bool has_error_{};
    int scroll_line_{};
};

}  // namespace

DWORD run_validation_window(
    HINSTANCE module,
    ValidationSession* session,
    RetailCaptureAgent* capture,
    bool writes_enabled,
    std::wstring window_identity,
    std::wstring startup_diagnostic) {
    ValidationWindow window(
        module,
        session,
        capture,
        writes_enabled,
        std::move(window_identity),
        std::move(startup_diagnostic));
    return window.run();
}

DWORD run_validation_window(
    HINSTANCE module,
    ValidationSession* session,
    std::wstring startup_diagnostic) {
    return run_validation_window(
        module,
        session,
        nullptr,
        false,
        {},
        std::move(startup_diagnostic));
}

}  // namespace opennova::retail_hook::windows
