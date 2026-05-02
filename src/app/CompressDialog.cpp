#include "stdafx.h"
#include "CompressDialog.h"
#include "flat_button.h"

#include <memory>
#include <new>

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

    // Apply localized caption + labels at runtime.
    CString s;
    s.LoadString(IDS_DIALOG_COMPRESS_TITLE);  SetWindowText(s);
    s.LoadString(IDS_LABEL_COMPRESS_READY);   SetDlgItemText(IDC_COMPRESS_CURRENT, s);
    s.LoadString(IDS_BUTTON_CANCEL);          SetDlgItemText(IDC_COMPRESS_CANCEL, s);

    if (auto* pb = static_cast<CProgressCtrl*>(GetDlgItem(IDC_COMPRESS_PROGRESS))) {
        pb->SetRange32(0, 1000);
        pb->SetPos(0);
    }

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
    return 0;
}

LRESULT CCompressDialog::OnBytes(WPARAM, LPARAM lp) {
    std::unique_ptr<BytesMsg> m(reinterpret_cast<BytesMsg*>(lp));
    if (m->total > 0) {
        int v = static_cast<int>((m->done * 1000) / m->total);
        if (auto* pb = static_cast<CProgressCtrl*>(GetDlgItem(IDC_COMPRESS_PROGRESS)))
            pb->SetPos(v);
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
