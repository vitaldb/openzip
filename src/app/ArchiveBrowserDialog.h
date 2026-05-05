#pragma once

#include "stdafx.h"
#include "resource.h"
#include "dark_theme.h"
#include "core/extractor.h"

#include <filesystem>
#include <set>
#include <vector>

// CArchiveBrowserDialog — shown on .zip double-click (file association).
//
// The flat ZIP central directory is rendered as a hierarchical tree:
// folders show ▶ / ▼ glyphs and can be expanded inline; descendants
// are indented per depth. Selection drives "Extract" (selected entries
// only, recursively for folders); "Extract All" with no selection
// extracts everything. File double-click extracts that one entry to a
// chosen folder.

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

    afx_msg void   OnExtractAll();
    afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);
    afx_msg void   OnListClick(NMHDR* hdr, LRESULT* result);
    afx_msg void   OnListDoubleClick(NMHDR* hdr, LRESULT* result);
    afx_msg void   OnListItemChanged(NMHDR* hdr, LRESULT* result);
    afx_msg void   OnListCustomDraw(NMHDR* hdr, LRESULT* result);
    afx_msg void   OnListRClick(NMHDR* hdr, LRESULT* result);
    afx_msg void   OnListColumnClick(NMHDR* hdr, LRESULT* result);
    afx_msg void   OnListBeginDrag(NMHDR* hdr, LRESULT* result);
    afx_msg void   OnSize(UINT nType, int cx, int cy);
    afx_msg void   OnGetMinMaxInfo(MINMAXINFO* mmi);
    afx_msg void   OnPaint();
    DECLARE_MESSAGE_MAP()

    void RelayoutChildren(int cx, int cy);
    void UpdateSortIndicator();

private:
    // ── Tree model ──────────────────────────────────────────────────
    // Each visible row in the list view corresponds to a Row entry.
    struct Row {
        std::wstring full_path;   // archive-relative path (no leading /)
        std::wstring leaf;        // last segment (display label)
        int          depth = 0;
        bool         is_dir = false;
        bool         is_expanded = false;
        // Aggregate stats for folders (sum of all descendant files);
        // for files, copies of Entry's own values.
        uint64_t     uncompressed_size = 0;
        uint64_t     compressed_size = 0;
        std::time_t  modified_time = 0;
        std::time_t  created_time  = 0;
    };

    void RebuildVisibleRows();   // builds visible_rows_ from entries_ + expanded_folders_
    void RenderRows();           // pushes visible_rows_ into the listview
    void UpdateButtonLabel();
    void ToggleFolderAt(int row_index);

    bool PickDestinationFolder(std::wstring& out);
    void ExpandRowToEntryNames(const std::wstring& row_full_path,
                               bool row_is_dir,
                               std::vector<std::wstring>& out_set) const;

    // Extract `entry_names` (already expanded) to a fresh subdirectory under
    // %TEMP%/openzip-preview/, using a modal CExtractDialog so the user sees
    // progress and is prompted for passwords / conflicts. Returns the temp
    // directory on success, empty path on cancel/failure.
    //
    // Used by both double-click "open" and the lazy CF_HDROP drag-out
    // callback — the latter invokes this from inside IDataObject::GetData
    // (which fires only at drop time, so a modal dialog is safe there).
    std::filesystem::path ExtractEntriesToTempModal(
            const std::vector<std::wstring>& entry_names);

    // Sweep stale preview folders (older than 24h) under %TEMP%/openzip-preview/.
    // Best-effort; failures are silently ignored — Windows will reuse the
    // space when needed.
    void CleanupOldPreviewDirs();

    std::vector<openzip::Extractor::Entry> entries_;
    std::set<std::wstring>                 expanded_folders_;
    std::vector<Row>                       visible_rows_;
    CListCtrl                              list_;
    HIMAGELIST                             sys_images_ = nullptr;  // not owned; do not free
    int                                    icon_w_ = 16;
    int                                    icon_h_ = 16;

    // Sort state. Column index matches InsertColumn order:
    //   0 = Name, 1 = Size, 2 = Packed, 3 = Modified, 4 = Created.
    // The hierarchical DFS order + folders-before-files-at-each-level
    // invariants are always preserved; the column choice only changes
    // the within-type sibling order at each tree level.
    int  sort_column_     = 0;
    bool sort_descending_ = false;
};
