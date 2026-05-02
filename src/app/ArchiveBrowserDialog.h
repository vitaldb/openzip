#pragma once

#include "stdafx.h"
#include "resource.h"
#include "dark_theme.h"
#include "core/extractor.h"

#include <filesystem>
#include <vector>

// CArchiveBrowserDialog — shown when the user double-clicks a .zip in Explorer
// (file-association invocation: openzip.exe <zip>).
//
// Caller sets `zip_path` before calling DoModal().
//
// DoModal() returns IDOK     → caller should run CExtractDialog. The dialog
//                              has populated `chosen_extract_dir` (target) and
//                              `chosen_filter_names` (entries to extract;
//                              empty = all).
// DoModal() returns IDCANCEL → user closed the browser without extracting.

class CArchiveBrowserDialog : public CDialogEx {
    DECLARE_DYNAMIC(CArchiveBrowserDialog)
public:
    explicit CArchiveBrowserDialog(CWnd* pParent = nullptr);
    enum { IDD = IDD_ARCHIVE_BROWSER };

    // Caller fills before DoModal:
    std::filesystem::path zip_path;

    // Outputs (valid when DoModal returned IDOK):
    std::filesystem::path     chosen_extract_dir;
    std::vector<std::wstring> chosen_filter_names;  // empty = "extract all"

protected:
    BOOL OnInitDialog() override;
    void DoDataExchange(CDataExchange* pDX) override;

    afx_msg void OnExtractAll();
    afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);
    afx_msg void OnListDoubleClick(NMHDR* hdr, LRESULT* result);
    afx_msg void OnListItemChanged(NMHDR* hdr, LRESULT* result);
    DECLARE_MESSAGE_MAP()

private:
    void PopulateList();
    void NavigateInto(const std::wstring& folder_name);
    void NavigateUp();
    void UpdateButtonLabel();

    // Show SHBrowseForFolderW for choosing the extraction destination.
    // Returns true and fills `out` on OK; false on cancel.
    bool PickDestinationFolder(std::wstring& out);

    // Expand a row (file or folder, real or synthetic) into the list of full
    // archive entry names that should be extracted for it. For folders this
    // includes the folder entry itself (if present) plus all descendants.
    void ExpandRowToEntryNames(const std::wstring& row_full_path,
                               bool row_is_dir,
                               std::vector<std::wstring>& out_set) const;

    std::vector<openzip::Extractor::Entry> entries_;  // cached list of zip contents
    std::wstring current_dir_;                        // empty = root, otherwise ends with '/'
    CListCtrl    list_;                                // wired to IDC_BROWSE_LIST
};
