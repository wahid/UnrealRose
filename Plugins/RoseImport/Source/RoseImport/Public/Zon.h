#pragma once

#include "Common.h"

class Zon {
public:
    Zon(const TCHAR* Filename) {
        FFileHelper::LoadFileToArray(rh.data, Filename);
    }

private:
	ReadHelper rh;
};
