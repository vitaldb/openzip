#pragma once

#include "stdafx.h"

class COpenZipApp : public CWinApp {
public:
    COpenZipApp() = default;
    BOOL InitInstance() override;

    DECLARE_MESSAGE_MAP()
};

extern COpenZipApp theApp;
