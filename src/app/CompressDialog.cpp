#include "stdafx.h"
#include "CompressDialog.h"
#include "flat_button.h"

#include <memory>
#include <new>

namespace {

// Format an estimated-remaining-time in milliseconds as either "M:SS" or
// "H:MM:SS" (drop the hours field below 1 hour). Used purely for display.
CString FormatEtaTime(uint64_t ms) {
    uint64_t secs = (ms + 500) / 1000;
    uint64_t h = secs / 3600;
    uint64_t m = (secs / 60) % 60;
    uint64_t s = secs % 60;
    CString out;
    if (h > 0) {
        out.Format(L"%llu:%02llu:%02llu",
                   static_cast<unsigned long long>(h),
                   static_cast<unsigned long long>(m),
                   static_cast<unsigned long long>(s));
    } else {
        out.Format(L"%llu:%02llu",
                   static_cast<unsigned long long>(m),
                   static_cast<unsigned long long>(s));
    }
    return out;
}

}  // namespace

// Custom window messages for worker → UI thread communication.
static constexpr UINT WM_OZ_COMP_ENTRY    = WM_USER + 200;
static constexpr UINT WM_OZ_COMP_BYTES    = WM_USER + 201;
static constexpr UINT WM_OZ_COMP_COMPLETE = WM_USER + 202;

namespace {

struct EntryMsg { std::wstring rel; size_t i, total; };
struct BytesMsg { uint64_t done, total; };

// ProgressCallback implementation that posts messages back to the dialog.
class DialogCallback : public openzip::Compressor::ProgressCallback {
public:
    explicit DialogCallback(HWND hwnd, std::atomic<bool>* cancel)
        : hwnd_(hwnd), cancel_(cancel) {}

    void OnEntryStart(const std::wstring& rel, size_t i, size_t total) override {
        auto* m = new (std::nothrow) EntryMsg{rel, i, total};
        if (m) ::PostMessageW(hwnd_, WM_OZ_COMP_ENTRY, 0, reinterpret_cast<LPARAM>(m));
    }

    void OnBytes(uint64_t done, uint64_t total) override {
        auto* m = new (std::nothrow) BytesMsg{done, total};
        if (m) ::PostMessageW(hwnd_, WM_OZ_COMP_BYTES, 0, reinterpret_cast<LPARAM>(m));
    }

    openzip::Extractor::ConflictAction OnOutputExists(
            const std::filesystem::path& /*output_zip*/) override {
        // For v0.3, batch/bundle mode auto-overwrites;
        // interactive mode has CCompressOptionsDialog run before the queue.
        return openzip::Extractor::ConflictAction::Overwrite;
    }

    bool ShouldCancel() override { return cancel_->load(); }

    void OnComplete(openzip::Compressor::Result r) override {
        ::PostMessageW(hwnd_, WM_OZ_COMP_COMPLETE,
                       static_cast<WPARAM>(r), 0);
    }

private:
    HWND hwnd_;
    std::atomic<bool>* cancel_;
};

}  // namespace

IMPLEMENT_DYNAMIC(CCompressDialog, CDialogEx)

CCompressDialog::CCompressDialog(CWnd* pParent)
    : CDialogEx(IDD, pParent) {}

BEGIN_MESSAGE_MAP(CCompressDialog, CDialogEx)
    ON_MESSAGE(WM_OZ_COMP_ENTRY,    &CCompressDialog::OnEntryStart)
    ON_MESSAGE(WM_OZ_COMP_BYTES,    &CCompressDialog::OnBytes)
    ON_MESSAGE(WM_OZ_COMP_COMPLETE, &CCompressDialog::OnComplete)
    ON_WM_CTLCOLOR()
    ON_NOTIFY(NM_CUSTOMDRAW, IDC_COMPRESS_PROGRESS, &CCompressDialog::OnProgressCustomDraw)
END_MESSAGE_MAP()

void CCompressDialog::DoDataExchange(CDataExchange* pDX) {
    CDialogEx::DoDataExchange(pDX);
}

BOOL CCompressDialog::OnInitDialog() {
    CDialogEx::OnInitDialog();

    if (openzip::dark_theme::IsDarkModeActive()) {
        openzip::dark_theme::EnableForWindow(GetSafeHwnd());
    }
    openzip::flat::ApplyToDialog(GetSafeHwnd());
    openzip::icons::ApplyDialogIcon(GetSafeHwnd(), AfxGetResourceHandle(), IDR_MAINFRAME);

    // Apply localized caption + labels at runtime.
    CString s;
    s.LoadString(IDS_DIALOG_COMPRESS_TITLE);  SetWindowText(s);
    s.LoadString(IDS_LABEL_COMPRESS_READY);   SetDlgItemText(IDC_COMPRESS_CURRENT, s);
    s.LoadString(IDS_BUTTON_CANCEL);          SetDlgItemText(IDC_COMPRESS_CANCEL, s);

    if (auto* pb = static_cast<CProgressCtrl*>(GetDlgItem(IDC_COMPRESS_PROGRESS))) {
        pb->SetRange32(0, 1000);
        pb->SetPos(0);
    }

    // Initial label state — bar at 0%, no ETA available yet.
    SetDlgItemText(IDC_COMPRESS_PERCENT, L"0%");
    SetDlgItemText(IDC_COMPRESS_COUNT,   L"");
    s.LoadString(IDS_LABEL_COMPRESS_ETA_UNKNOWN);
    SetDlgItemText(IDC_COMPRESS_ETA, s);

    if (batch_total > 1) {
        s.Format(L"%d / %d", batch_index + 1, batch_total);
        SetDlgItemText(IDC_COMPRESS_QUEUE, s);
    }

    // Start the worker thread.
    worker_ = std::thread([this] { RunWorker(); });
    return TRUE;
}

void CCompressDialog::RunWorker() {
    DialogCallback cb(GetSafeHwnd(), &cancel_);
    final_result_ = openzip::Compressor::Compress(sources, output_path, cb, options);
}

void CCompressDialog::OnCancel() {
    cancel_.store(true);
    CString cancelling;
    cancelling.LoadString(IDS_LABEL_CANCELLING);
    SetDlgItemText(IDC_COMPRESS_CANCEL, cancelling);
    if (CWnd* btn = GetDlgItem(IDC_COMPRESS_CANCEL))
        btn->EnableWindow(FALSE);
    // Don't call base OnCancel — let WM_OZ_COMP_COMPLETE close the dialog
    // so EndDialog is only called after the worker acknowledges cancellation.
}

LRESULT CCompressDialog::OnEntryStart(WPARAM, LPARAM lp) {
    std::unique_ptr<EntryMsg> m(reinterpret_cast<EntryMsg*>(lp));
    SetDlgItemText(IDC_COMPRESS_CURRENT, m->rel.c_str());

    entry_index_ = m->i;
    entry_total_ = m->total;

    // Refresh "N / M files" alongside the path so it's never stale on small entries.
    CString fmt;
    fmt.LoadString(IDS_LABEL_COMPRESS_FILES);
    CString count;
    count.Format(fmt,
                 static_cast<unsigned int>(entry_index_ + 1),
                 static_cast<unsigned int>(entry_total_));
    SetDlgItemText(IDC_COMPRESS_COUNT, count);
    return 0;
}

LRESULT CCompressDialog::OnBytes(WPARAM, LPARAM lp) {
    std::unique_ptr<BytesMsg> m(reinterpret_cast<BytesMsg*>(lp));

    DWORD now = ::GetTickCount();
    if (start_tick_ == 0) start_tick_ = now;

    // Bar moves at high resolution; the worker can post much faster than the
    // UI can repaint, so cap bar updates to ~60Hz.
    if (m->total > 0) {
        int v = static_cast<int>((m->done * 1000) / m->total);
        if (auto* pb = static_cast<CProgressCtrl*>(GetDlgItem(IDC_COMPRESS_PROGRESS)))
            pb->SetPos(v);
    }

    // Text labels (percent / ETA) refresh at most ~4×/sec to avoid flicker
    // and to give the ETA estimator enough samples between recomputes.
    bool finished = (m->total > 0 && m->done >= m->total);
    if (!finished && now - last_label_tick_ < 250) return 0;
    last_label_tick_ = now;

    if (m->total > 0) {
        double promille = static_cast<double>(m->done * 1000) / static_cast<double>(m->total);
        CString pct;
        pct.Format(L"%.1f%%", promille / 10.0);
        SetDlgItemText(IDC_COMPRESS_PERCENT, pct);

        // ETA: linear extrapolation from total elapsed throughput. Suppress
        // the estimate until we have ≥ 750 ms of data and ≥ 0.5 % progress —
        // earlier samples are dominated by setup overhead and would produce
        // wild numbers (hours of "ETA" on a 2-second job).
        DWORD elapsed = now - start_tick_;
        CString eta;
        bool have_estimate = false;
        uint64_t eta_ms = 0;
        if (finished) {
            have_estimate = true;  // eta_ms stays 0 → "0:00"
        } else if (m->done > 0 && elapsed >= 750 && promille >= 5.0) {
            uint64_t remaining_bytes = m->total - m->done;
            eta_ms = static_cast<uint64_t>(elapsed) * remaining_bytes / m->done;
            // Clamp absurd values so we never paint "ETA 999:59:59".
            constexpr uint64_t kEtaCapMs = 24ull * 60ull * 60ull * 1000ull;
            if (eta_ms > kEtaCapMs) eta_ms = kEtaCapMs;
            have_estimate = true;
        }
        if (have_estimate) {
            CString fmt;
            fmt.LoadString(IDS_LABEL_COMPRESS_ETA);
            eta.Format(fmt, static_cast<LPCWSTR>(FormatEtaTime(eta_ms)));
        } else {
            eta.LoadString(IDS_LABEL_COMPRESS_ETA_UNKNOWN);
        }
        SetDlgItemText(IDC_COMPRESS_ETA, eta);
    }
    return 0;
}

LRESULT CCompressDialog::OnComplete(WPARAM wp, LPARAM) {
    final_result_ = static_cast<openzip::Compressor::Result>(wp);
    if (worker_.joinable()) worker_.join();
    EndDialog(final_result_ == openzip::Compressor::Result::Success ? IDOK : IDABORT);
    return 0;
}

HBRUSH CCompressDialog::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor) {
    UINT msg;
    switch (nCtlColor) {
        case CTLCOLOR_EDIT:   msg = WM_CTLCOLOREDIT;   break;
        case CTLCOLOR_STATIC: msg = WM_CTLCOLORSTATIC; break;
        case CTLCOLOR_BTN:    msg = WM_CTLCOLORBTN;    break;
        default:              msg = WM_CTLCOLORDLG;    break;
    }
    if (HBRUSH b = openzip::dark_theme::OnCtlColor(
            pWnd->GetSafeHwnd(), pDC->GetSafeHdc(), msg))
        return b;
    return CDialogEx::OnCtlColor(pDC, pWnd, nCtlColor);
}

void CCompressDialog::OnProgressCustomDraw(NMHDR* hdr, LRESULT* result) {
    *result = openzip::dark_theme::OnProgressCustomDraw(hdr);
}
