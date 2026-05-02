#pragma once

#include "stdafx.h"
#include "resource.h"
#include "core/compressor.h"
#include "dark_theme.h"

#include <atomic>
#include <filesystem>
#include <thread>
#include <vector>

class CCompressDialog : public CDialogEx {
    DECLARE_DYNAMIC(CCompressDialog)
public:
    explicit CCompressDialog(CWnd* pParent = nullptr);
    enum { IDD = IDD_COMPRESS };

    // Caller fills these before DoModal.
    std::vector<std::filesystem::path> sources;
    std::filesystem::path output_path;
    openzip::Compressor::Options options;

    // For multi-archive batches: 0-based index and total count.
    // Pass {0, 1} (or leave default) for a single-archive compress.
    int batch_index = 0;
    int batch_total = 1;

    openzip::Compressor::Result final_result() const { return final_result_; }

protected:
    BOOL OnInitDialog() override;
    void DoDataExchange(CDataExchange* pDX) override;
    void OnCancel() override;

    afx_msg LRESULT OnEntryStart(WPARAM wp, LPARAM lp);
    afx_msg LRESULT OnBytes(WPARAM wp, LPARAM lp);
    afx_msg LRESULT OnComplete(WPARAM wp, LPARAM lp);
    afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);
    afx_msg void OnProgressCustomDraw(NMHDR* hdr, LRESULT* result);
    DECLARE_MESSAGE_MAP()

private:
    void RunWorker();

    std::thread worker_;
    std::atomic<bool> cancel_{false};
    openzip::Compressor::Result final_result_ = openzip::Compressor::Result::Success;
};
