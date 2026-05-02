#pragma once

// ─── Dialog IDs ─────────────────────────────────────────────────
#define IDD_EXTRACT                     101
#define IDD_PASSWORD                    102
#define IDD_CONFLICT                    103

// ─── Extract dialog controls ────────────────────────────────────
#define IDC_LABEL_ARCHIVE               1001
#define IDC_LABEL_CURRENT_FILE          1002
#define IDC_PROGRESS_OVERALL            1003
#define IDC_LABEL_PERCENT               1004
#define IDC_LABEL_SPEED                 1005
#define IDC_LABEL_COUNT                 1006
#define IDC_LABEL_ARCHIVE_HEADING       1007
#define IDC_LABEL_CURRENT_HEADING       1008

// ─── Password dialog controls ───────────────────────────────────
#define IDC_LABEL_PASSWORD_PROMPT       1101
#define IDC_EDIT_PASSWORD               1102
#define IDC_LABEL_PASSWORD_HINT         1103
#define IDC_LABEL_PASSWORD_ENTER        1104

// ─── Conflict dialog controls ───────────────────────────────────
#define IDC_LABEL_CONFLICT_PATH         1201
#define IDC_CHECK_REMEMBER              1202
#define IDC_BTN_OVERWRITE               1203
#define IDC_BTN_SKIP                    1204
#define IDC_BTN_RENAME                  1205
#define IDC_LABEL_CONFLICT_HEADING      1206

// ─── String table — UI text used at runtime via LoadString ──────
#define IDS_DIALOG_EXTRACT_TITLE        2001
#define IDS_DIALOG_PASSWORD_TITLE       2002
#define IDS_DIALOG_CONFLICT_TITLE       2003

#define IDS_LABEL_ARCHIVE               2010
#define IDS_LABEL_CURRENT               2011
#define IDS_LABEL_PASSWORD_ENTER        2012
#define IDS_LABEL_PASSWORD_ENC_FOR      2013   // "Archive '%s' is password-protected."
#define IDS_LABEL_PASSWORD_ENC_ENTRY    2014   // "'%s' inside '%s' is encrypted."
#define IDS_LABEL_PASSWORD_HINT_WRONG   2015
#define IDS_LABEL_CONFLICT              2016
#define IDS_CHECK_REMEMBER              2017
#define IDS_LABEL_CANCELLING            2018

#define IDS_BUTTON_CANCEL               2030
#define IDS_BUTTON_OK                   2031
#define IDS_BUTTON_OVERWRITE            2032
#define IDS_BUTTON_SKIP                 2033
#define IDS_BUTTON_RENAME               2034

#define IDS_HELP_TEXT                   2050
#define IDS_HELP_TITLE                  2051
#define IDS_ERROR_TITLE                 2052
