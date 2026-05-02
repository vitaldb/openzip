#pragma once

// ─── App icon (MFC convention) ──────────────────────────────────
// CDialogEx::OnInitDialog auto-loads IDR_MAINFRAME for the dialog's
// big/small icons via AfxGetApp()->LoadIcon, fixing the otherwise
// generic-looking taskbar icon.
#define IDR_MAINFRAME                   128

// ─── Dialog IDs ─────────────────────────────────────────────────
#define IDD_EXTRACT                     101
#define IDD_PASSWORD                    102
#define IDD_CONFLICT                    103
#define IDD_COMPRESS_OPTIONS            140
#define IDD_COMPRESS                    141

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

// ─── Compress options dialog controls ───────────────────────────
#define IDC_COMPRESS_OUTNAME            2001
#define IDC_COMPRESS_OUTDIR             2002
#define IDC_COMPRESS_BROWSE             2003
#define IDC_COMPRESS_LEVEL_STORE        2004
#define IDC_COMPRESS_LEVEL_FAST         2005
#define IDC_COMPRESS_LEVEL_NORMAL       2006
#define IDC_COMPRESS_LEVEL_MAX          2007
#define IDC_COMPRESS_PASSWORD           2008
#define IDC_COMPRESS_PASSWORD_CFM       2009
#define IDC_COMPRESS_SHOW_PW            2010

// Static labels — distinct IDs so OnInitDialog can localize them.
#define IDC_LABEL_COMPRESS_OUTNAME      2030
#define IDC_LABEL_COMPRESS_OUTDIR       2031
#define IDC_GROUP_COMPRESS_LEVEL        2032
#define IDC_LABEL_COMPRESS_PASSWORD     2033
#define IDC_LABEL_COMPRESS_PASSWORD_CFM 2034

// ─── Compress progress dialog controls ──────────────────────────
#define IDC_COMPRESS_CURRENT            2020
#define IDC_COMPRESS_PROGRESS           2021
#define IDC_COMPRESS_QUEUE              2022
#define IDC_COMPRESS_CANCEL             2023

// ─── String table — UI text used at runtime via LoadString ──────
#define IDS_DIALOG_EXTRACT_TITLE        4001
#define IDS_DIALOG_PASSWORD_TITLE       4002
#define IDS_DIALOG_CONFLICT_TITLE       4003

#define IDS_LABEL_ARCHIVE               4010
#define IDS_LABEL_CURRENT               4011
#define IDS_LABEL_PASSWORD_ENTER        4012
#define IDS_LABEL_PASSWORD_ENC_FOR      4013   // "Archive '%s' is password-protected."
#define IDS_LABEL_PASSWORD_ENC_ENTRY    4014   // "'%s' inside '%s' is encrypted."
#define IDS_LABEL_PASSWORD_HINT_WRONG   4015
#define IDS_LABEL_CONFLICT              4016
#define IDS_CHECK_REMEMBER              4017
#define IDS_LABEL_CANCELLING            4018

#define IDS_BUTTON_CANCEL               4030
#define IDS_BUTTON_OK                   4031
#define IDS_BUTTON_OVERWRITE            4032
#define IDS_BUTTON_SKIP                 4033
#define IDS_BUTTON_RENAME               4034

#define IDS_HELP_TEXT                   4050
#define IDS_HELP_TITLE                  4051
#define IDS_ERROR_TITLE                 4052

// Compress dialogs (Phase 3)
#define IDS_DIALOG_COMPRESS_OPT_TITLE   4060
#define IDS_DIALOG_COMPRESS_TITLE       4061
#define IDS_LABEL_COMPRESS_OUTNAME      4062
#define IDS_LABEL_COMPRESS_OUTDIR       4063
#define IDS_GROUP_COMPRESS_LEVEL        4064
#define IDS_RADIO_COMPRESS_LEVEL_STORE  4065
#define IDS_RADIO_COMPRESS_LEVEL_FAST   4066
#define IDS_RADIO_COMPRESS_LEVEL_NORMAL 4067
#define IDS_RADIO_COMPRESS_LEVEL_MAX    4068
#define IDS_LABEL_COMPRESS_PASSWORD     4069
#define IDS_LABEL_COMPRESS_PASSWORD_CFM 4070
#define IDS_CHECK_COMPRESS_SHOW_PW      4071
#define IDS_LABEL_COMPRESS_READY        4072

// Archive browser dialog (Phase 9)
#define IDD_ARCHIVE_BROWSER             142
#define IDC_BROWSE_ZIPPATH              2050
#define IDC_BROWSE_LIST                 2051
#define IDC_BROWSE_EXTRACT_ALL          2052
#define IDS_DIALOG_BROWSE_TITLE         4080
#define IDS_LABEL_BROWSE_NAME           4081
#define IDS_LABEL_BROWSE_SIZE           4082
#define IDS_BUTTON_EXTRACT_ALL          4084
#define IDS_BUTTON_CLOSE                4085
#define IDS_BUTTON_EXTRACT_SELECTED     4086
#define IDS_PICK_DESTINATION            4087
