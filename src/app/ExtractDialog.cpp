#include "stdafx.h"
#include "ExtractDialog.h"

#include "PasswordDialog.h"
#include "ConflictDialog.h"

#include <chrono>

IMPLEMENT_DYNAMIC(CExtractDialog, CDialogEx)

BEGIN_MESSAGE_MAP(CExtractDialog, CDialogEx)
    ON_MESSAGE(WM_OPENZIP_PROMPT_PASSWORD, &CExtractDialog::OnPromptPassword)
    ON_MESSAGE(WM_OPENZIP_PROMPT_CONFLICT, &CExtractDialog::OnPromptConflict)
    ON_MESSAGE(WM_OPENZIP_ENTRY_START,     &CExtractDialog::OnEntryStartMsg)
    ON_MESSAGE(WM_OPENZIP_BYTES,           &CExtractDialog::OnBytesMsg)
    ON_MESSAGE(WM_OPENZIP_COMPLETE,        &CExtractDialog::OnCompleteMsg)
END_MESSAGE_MAP()

CExtractDialog::CExtractDialog(const openzip::CommandLine& cmd, CWnd* parent)
    : CDialogEx(IDD_EXTRACT, parent), cmd_(cmd) {}

void CExtractDialog::DoDataExchange(CDataExchange* pDX) {
    CDialogEx::DoDataExchange(pDX);
}

BOOL CExtractDialog::OnInitDialog() {
    CDialogEx::OnInitDialog();

    SetDlgItemText(IDC_LABEL_ARCHIVE, cmd_.zip_path.wstring().c_str());
    SetDlgItemText(IDC_LABEL_CURRENT_FILE, L"");
    SetDlgItemText(IDC_LABEL_PERCENT, L"0%");
    SetDlgItemText(IDC_LABEL_SPEED, L"");
    SetDlgItemText(IDC_LABEL_COUNT, L"");

    auto* pb = static_cast<CProgressCtrl*>(GetDlgItem(IDC_PROGRESS_OVERALL));
    if (pb) {
        pb->SetRange32(0, 1000);  // 0.1% granularity
        pb->SetPos(0);
    }

    speed_window_start_tick_ = ::GetTickCount();
    speed_window_start_bytes_ = 0;

    worker_ = AfxBeginThread(WorkerProc, this, THREAD_PRIORITY_NORMAL,
                             0, CREATE_SUSPENDED);
    if (!worker_) {
        result_ = openzip::Extractor::Result::IoError;
        PostMessage(WM_OPENZIP_COMPLETE, static_cast<WPARAM>(result_.load()), 0);
        return TRUE;
    }
    worker_->m_bAutoDelete = TRUE;
    worker_->ResumeThread();

    return TRUE;
}

void CExtractDialog::OnCancel() {
    cancel_requested_ = true;
    SetDlgItemText(IDC_LABEL_CURRENT_FILE, L"Cancelling...");
    GetDlgItem(IDCANCEL)->EnableWindow(FALSE);
    // The worker thread will observe the flag, return Result::Cancelled,
    // and post WM_OPENZIP_COMPLETE which closes the dialog.
}

UINT CExtractDialog::WorkerProc(LPVOID p) {
    auto* dlg = static_cast<CExtractDialog*>(p);
    auto r = openzip::Extractor::Extract(dlg->cmd_.zip_path, dlg->cmd_.target_dir, *dlg);
    dlg->result_ = r;
    return 0;
}

// ---- ProgressCallback (worker thread) ----

void CExtractDialog::OnEntryStart(const openzip::Extractor::Entry& entry,
                                  size_t index, size_t total) {
    auto* args = new EntryStartArgs{entry.name, index, total};
    if (!PostMessage(WM_OPENZIP_ENTRY_START, 0, reinterpret_cast<LPARAM>(args))) {
        delete args;  // window already closed
    }
}

void CExtractDialog::OnBytes(uint64_t done, uint64_t total) {
    // Pack bytes_done into LPARAM as uint64 — but LPARAM is 64-bit on x64.
    PostMessage(WM_OPENZIP_BYTES, static_cast<WPARAM>(done), static_cast<LPARAM>(total));
}

std::wstring CExtractDialog::OnPasswordRequired(const std::wstring& archive_name,
                                                const std::wstring& entry_name,
                                                bool was_wrong) {
    // If the user pre-supplied a password and we haven't tried it yet, use it.
    if (!was_wrong && !cmd_.password.empty()) return cmd_.password;

    PasswordPromptArgs args{archive_name, entry_name, was_wrong, L"", false};
    SendMessage(WM_OPENZIP_PROMPT_PASSWORD, 0, reinterpret_cast<LPARAM>(&args));
    if (args.cancelled) return std::wstring();
    return args.result_password;
}

openzip::Extractor::ConflictAction CExtractDialog::OnFileConflict(const std::wstring& dest_path) {
    if (has_remembered_) return remembered_conflict_;

    ConflictPromptArgs args{dest_path, openzip::Extractor::ConflictAction::Cancel, false};
    SendMessage(WM_OPENZIP_PROMPT_CONFLICT, 0, reinterpret_cast<LPARAM>(&args));
    if (args.remember) {
        remembered_conflict_ = args.result_action;
        has_remembered_ = true;
    }
    return args.result_action;
}

bool CExtractDialog::ShouldCancel() {
    return cancel_requested_.load();
}

void CExtractDialog::OnComplete(openzip::Extractor::Result r) {
    result_ = r;
    PostMessage(WM_OPENZIP_COMPLETE, static_cast<WPARAM>(r), 0);
}

// ---- UI-thread message handlers ----

LRESULT CExtractDialog::OnPromptPassword(WPARAM, LPARAM lp) {
    auto* args = reinterpret_cast<PasswordPromptArgs*>(lp);

    CString archive(args->archive.c_str());
    CString entry(args->entry.c_str());
    CPasswordDialog dlg(archive, entry, args->was_wrong, this);
    if (dlg.DoModal() == IDOK) {
        args->result_password = static_cast<const wchar_t*>(dlg.password());
        args->cancelled = false;
    } else {
        args->result_password.clear();
        args->cancelled = true;
    }
    return 0;
}

LRESULT CExtractDialog::OnPromptConflict(WPARAM, LPARAM lp) {
    auto* args = reinterpret_cast<ConflictPromptArgs*>(lp);

    CConflictDialog dlg(CString(args->dest_path.c_str()), this);
    dlg.DoModal();
    args->result_action = dlg.action();
    args->remember = dlg.remember();
    return 0;
}

LRESULT CExtractDialog::OnEntryStartMsg(WPARAM, LPARAM lp) {
    std::unique_ptr<EntryStartArgs> args(reinterpret_cast<EntryStartArgs*>(lp));
    SetDlgItemText(IDC_LABEL_CURRENT_FILE, args->name.c_str());
    CString count;
    count.Format(L"%zu / %zu", args->index + 1, args->total);
    SetDlgItemText(IDC_LABEL_COUNT, count);
    return 0;
}

LRESULT CExtractDialog::OnBytesMsg(WPARAM wp, LPARAM lp) {
    uint64_t done = static_cast<uint64_t>(wp);
    uint64_t total = static_cast<uint64_t>(lp);

    DWORD now = ::GetTickCount();

    // Throttle UI updates to ~60Hz; the worker thread can post much faster.
    if (now - last_paint_tick_ < 16 && done < total) return 0;
    last_paint_tick_ = now;

    if (total > 0) {
        int promille = static_cast<int>((done * 1000) / total);
        auto* pb = static_cast<CProgressCtrl*>(GetDlgItem(IDC_PROGRESS_OVERALL));
        if (pb) pb->SetPos(promille);
        CString pct;
        pct.Format(L"%.1f%%", promille / 10.0);
        SetDlgItemText(IDC_LABEL_PERCENT, pct);
    }

    // Speed averaged over a 1-second window.
    if (speed_window_start_tick_ == 0) {
        speed_window_start_tick_ = now;
        speed_window_start_bytes_ = done;
    }
    DWORD elapsed = now - speed_window_start_tick_;
    if (elapsed >= 1000) {
        uint64_t window_bytes = done - speed_window_start_bytes_;
        double mb_per_sec = (window_bytes * 1000.0) / (elapsed * 1024.0 * 1024.0);
        CString s;
        s.Format(L"%.1f MB/s", mb_per_sec);
        SetDlgItemText(IDC_LABEL_SPEED, s);
        speed_window_start_tick_ = now;
        speed_window_start_bytes_ = done;
    }
    last_bytes_ = done;
    return 0;
}

LRESULT CExtractDialog::OnCompleteMsg(WPARAM wp, LPARAM) {
    auto r = static_cast<openzip::Extractor::Result>(wp);
    result_ = r;
    EndDialog(r == openzip::Extractor::Result::Success ? IDOK : IDCANCEL);
    return 0;
}
