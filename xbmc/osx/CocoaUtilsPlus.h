//
//  CocoaUtilsPlus.h
//  Plex
//
//  Created by Enrique Osuna on 10/26/2008.
//  Copyright 2008 __MyCompanyName__. All rights reserved.
//
#pragma once

#ifdef __APPLE__

#include <vector>
#include <string>
#include <sys/types.h>
#include "MediaSource.h"

using namespace std;

//
// Initialization
//
extern "C" void CocoaPlus_Initialize();

//
// Fonts
//
vector<string> Cocoa_GetSystemFonts();
string Cocoa_GetSystemFontPathFromDisplayName(const string displayName);

//
// Plex Media Server Services
//
vector<in_addr_t> Cocoa_AddressesForHost(const string& hostname);
bool Cocoa_AreHostsEqual(const string& host1, const string& host2);
VECSOURCES Cocoa_GetPlexMediaServersAsSourcesWithMediaType(const string& mediaType);
bool Cocoa_IsLocalPlexMediaServerRunning();

//
// Proxy Settings (continued)
//
vector<CStdString> Cocoa_Proxy_ExceptionList();

//
// Locale functions.
//
string Cocoa_GetCountryCode();
string Cocoa_GetLanguage();
string Cocoa_GetSimpleLanguage();
string Cocoa_ConvertIso6392ToIso6391(const string& lang);
bool   Cocoa_IsMetricSystem();
string Cocoa_GetLongDateFormat();
string Cocoa_GetShortDateFormat();
string Cocoa_GetTimeFormat(bool withMeridian=true);
string Cocoa_GetMeridianSymbol(int i);
string Cocoa_GetDateString(time_t time, bool longDate);
string Cocoa_GetTimeString(time_t time);
string Cocoa_GetTimeString(const string& format, time_t time);

//
// Address book functions.
//
string Cocoa_GetMyZip();
string Cocoa_GetMyCity();
string Cocoa_GetMyState();
string Cocoa_GetMyCountry();

//
// Misc.
//
string Cocoa_GetMachineSerialNumber();
string Cocoa_GetPrimaryMacAddress();

#endif
