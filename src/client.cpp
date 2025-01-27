/*
 *      Copyright (C) 2010 Alwin Esch (Team XBMC)
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "client.h"
#include "xbmc_pvr_dll.h"
#include "VNSIDemux.h"
#include "VNSIRecording.h"
#include "VNSIData.h"
#include "VNSIChannelScan.h"
#include "VNSIAdmin.h"
#include "vnsicommand.h"
#include "p8-platform/util/util.h"

#include <sstream>
#include <string>
#include <iostream>

using namespace std;
using namespace ADDON;
using namespace P8PLATFORM;

ADDON_STATUS m_CurStatus = ADDON_STATUS_UNKNOWN;

/* User adjustable settings are saved here.
 * Default values are defined inside client.h
 * and exported to the other source files.
 */
std::string   g_szHostname              = DEFAULT_HOST;
std::string   g_szWolMac                = "";
int           g_iPort                   = DEFAULT_PORT;
bool          g_bCharsetConv            = DEFAULT_CHARCONV;     ///< Convert VDR's incoming strings to UTF8 character set
int           g_iConnectTimeout         = DEFAULT_TIMEOUT;      ///< The Socket connection timeout
int           g_iPriority               = DEFAULT_PRIORITY;     ///< The Priority this client have in response to other clients
bool          g_bAutoChannelGroups      = DEFAULT_AUTOGROUPS;
int           g_iTimeshift              = 1;
std::string   g_szIconPath              = "";
int           g_iChunkSize              = DEFAULT_CHUNKSIZE;

int prioVals[] = {0,5,10,15,20,25,30,35,40,45,50,55,60,65,70,75,80,85,90,95,99,100};

CHelper_libXBMC_addon *XBMC = nullptr;
CHelper_libKODI_guilib *GUI = nullptr;
CHelper_libXBMC_pvr *PVR = nullptr;

cVNSIDemux *VNSIDemuxer = nullptr;
cVNSIData *VNSIData = nullptr;
cVNSIRecording *VNSIRecording = nullptr;

bool IsTimeshift;
bool IsRealtime;
int64_t PTSBufferEnd;
P8PLATFORM::CMutex TimeshiftMutex;

extern "C" {

/***********************************************************
 * Standart AddOn related public library functions
 ***********************************************************/

ADDON_STATUS ADDON_Create(void* hdl, void* props)
{
  if (!hdl || !props)
    return ADDON_STATUS_UNKNOWN;

  XBMC = new CHelper_libXBMC_addon;
  if (!XBMC->RegisterMe(hdl))
  {
    SAFE_DELETE(XBMC);
    return ADDON_STATUS_PERMANENT_FAILURE;
  }

  GUI = new CHelper_libKODI_guilib;
  if (!GUI->RegisterMe(hdl))
  {
    SAFE_DELETE(GUI);
    SAFE_DELETE(XBMC);
    return ADDON_STATUS_PERMANENT_FAILURE;
  }

  PVR = new CHelper_libXBMC_pvr;
  if (!PVR->RegisterMe(hdl))
  {
    SAFE_DELETE(PVR);
    SAFE_DELETE(GUI);
    SAFE_DELETE(XBMC);
    return ADDON_STATUS_PERMANENT_FAILURE;
  }

  XBMC->Log(LOG_DEBUG, "Creating VDR VNSI PVR-Client");

  m_CurStatus    = ADDON_STATUS_UNKNOWN;

  // Read setting "host" from settings.xml
  char * buffer = (char*) malloc(128);
  buffer[0] = 0;

  if (XBMC->GetSetting("host", buffer))
    g_szHostname = buffer;
  else
  {
    // If setting is unknown fallback to defaults
    XBMC->Log(LOG_ERROR, "Couldn't get 'host' setting, falling back to '%s' as default", DEFAULT_HOST);
    g_szHostname = DEFAULT_HOST;
  }
  free(buffer);

  buffer = (char*) malloc(64);
  buffer[0] = 0;

  // Read setting "wol_mac" from settings.xml
  if (XBMC->GetSetting("wol_mac", buffer))
    g_szWolMac = buffer;
  else
  {
    // If setting is unknown fallback to empty default
    XBMC->Log(LOG_ERROR, "Couldn't get 'wol_mac' setting, falling back to default");
    g_szWolMac = "";
  }
  free(buffer);

  // Read setting "port" from settings.xml
  if (!XBMC->GetSetting("port", &g_iPort))
  {
    // If setting is unknown fallback to defaults
    XBMC->Log(LOG_ERROR, "Couldn't get 'port' setting, falling back to '%i' as default", DEFAULT_PORT);
    g_iPort = DEFAULT_PORT;
  }

  // Read setting "priority" from settings.xml
  int prio = DEFAULT_PRIORITY;
  if (!XBMC->GetSetting("priority", &prio))
  {
    // If setting is unknown fallback to defaults
    XBMC->Log(LOG_ERROR, "Couldn't get 'priority' setting, falling back to %i as default", -1);
    prio = DEFAULT_PRIORITY;
  }
  g_iPriority = prioVals[prio];

  /* Read setting "timeshift" from settings.xml */
  if (!XBMC->GetSetting("timeshift", &g_iTimeshift))
  {
    // If setting is unknown fallback to defaults
    XBMC->Log(LOG_ERROR, "Couldn't get 'timeshift' setting, falling back to %i as default", 1);
    g_iTimeshift = 1;
  }

  // Read setting "convertchar" from settings.xml
  if (!XBMC->GetSetting("convertchar", &g_bCharsetConv))
  {
    /* If setting is unknown fallback to defaults */
    XBMC->Log(LOG_ERROR, "Couldn't get 'convertchar' setting, falling back to 'false' as default");
    g_bCharsetConv = DEFAULT_CHARCONV;
  }

  // Read setting "timeout" from settings.xml
  if (!XBMC->GetSetting("timeout", &g_iConnectTimeout))
  {
    /* If setting is unknown fallback to defaults */
    XBMC->Log(LOG_ERROR, "Couldn't get 'timeout' setting, falling back to %i seconds as default", DEFAULT_TIMEOUT);
    g_iConnectTimeout = DEFAULT_TIMEOUT;
  }

  // Read setting "autochannelgroups" from settings.xml
  if (!XBMC->GetSetting("autochannelgroups", &g_bAutoChannelGroups))
  {
    // If setting is unknown fallback to defaults
    XBMC->Log(LOG_ERROR, "Couldn't get 'autochannelgroups' setting, falling back to 'false' as default");
    g_bAutoChannelGroups = DEFAULT_AUTOGROUPS;
  }

  // Read setting "iconpath" from settings.xml
  buffer = (char*) malloc(512);
  buffer[0] = 0; /* Set the end of string */

  if (XBMC->GetSetting("iconpath", buffer))
    g_szIconPath = buffer;
  else
  {
    // If setting is unknown fallback to defaults
    XBMC->Log(LOG_ERROR, "Couldn't get 'iconpath' setting");
    g_szIconPath = "";
  }
  free(buffer);

  // Read setting "chunksize" from settings.xml
  if (!XBMC->GetSetting("chunksize", &g_iChunkSize))
  {
    /* If setting is unknown fallback to defaults */
    XBMC->Log(LOG_ERROR, "Couldn't get 'chunksize' setting, falling back to %i as default", DEFAULT_CHUNKSIZE);
    g_iChunkSize = DEFAULT_CHUNKSIZE;
  }

  try
  {
    VNSIData = new cVNSIData;
    m_CurStatus = ADDON_STATUS_OK;
    if (!VNSIData->Start(g_szHostname, g_iPort, nullptr, g_szWolMac))
    {
      ADDON_Destroy();
      m_CurStatus = ADDON_STATUS_PERMANENT_FAILURE;
      return m_CurStatus;
    }
  }
  catch (std::exception e)
  {
    XBMC->Log(LOG_ERROR, "%s - %s", __FUNCTION__, e.what());
    ADDON_Destroy();
    m_CurStatus = ADDON_STATUS_LOST_CONNECTION;
    return m_CurStatus;
  }

  PVR_MENUHOOK hook;
  hook.iHookId = 1;
  hook.category = PVR_MENUHOOK_SETTING;
  hook.iLocalizedStringId = 30107;
  PVR->AddMenuHook(&hook);

  return m_CurStatus;
}

ADDON_STATUS ADDON_GetStatus()
{
  return m_CurStatus;
}

void ADDON_Destroy()
{
  if (VNSIDemuxer)
    SAFE_DELETE(VNSIDemuxer);

  if (VNSIRecording)
    SAFE_DELETE(VNSIRecording);

  if (VNSIData)
    SAFE_DELETE(VNSIData);

  if (PVR)
    SAFE_DELETE(PVR);

  if (GUI)
    SAFE_DELETE(GUI);

  if (XBMC)
    SAFE_DELETE(XBMC);

  m_CurStatus = ADDON_STATUS_UNKNOWN;
}

ADDON_STATUS ADDON_SetSetting(const char *settingName, const void *settingValue)
{
  string str = settingName;
  if (str == "host")
  {
    string tmp_sHostname;
    XBMC->Log(LOG_INFO, "Changed Setting 'host' from %s to %s", g_szHostname.c_str(), (const char*) settingValue);
    tmp_sHostname = g_szHostname;
    g_szHostname = (const char*) settingValue;
    if (tmp_sHostname != g_szHostname)
      return ADDON_STATUS_NEED_RESTART;
  }
  else if (str == "wol_mac")
  {
    XBMC->Log(LOG_INFO, "Changed Setting 'wol_mac'");
    string tmp_sWol_mac;
    XBMC->Log(LOG_INFO, "Changed Setting 'wol_mac' from %s to %s", g_szWolMac.c_str(), (const char*) settingValue);
    tmp_sWol_mac = g_szWolMac;
    g_szWolMac = (const char*) settingValue;
    if (tmp_sWol_mac != g_szWolMac)
      return ADDON_STATUS_NEED_RESTART;
  }
  else if (str == "port")
  {
    XBMC->Log(LOG_INFO, "Changed Setting 'port' from %u to %u", g_iPort, *(int*)settingValue);
    if (g_iPort != *(int*)settingValue)
    {
      g_iPort = *(int*)settingValue;
      return ADDON_STATUS_NEED_RESTART;
    }
  }
  else if (str == "priority")
  {
    int newPrio = prioVals[*(int*)settingValue];
    XBMC->Log(LOG_INFO, "Changed Setting 'priority' from %u to %u", g_iPriority, newPrio);
    g_iPriority = newPrio;
  }
  else if (str == "timeshift")
  {
    XBMC->Log(LOG_INFO, "Changed Setting 'timeshift' from %u to %u", g_iTimeshift, *(int*) settingValue);
    g_iTimeshift = *(int*) settingValue;
  }
  else if (str == "convertchar")
  {
    XBMC->Log(LOG_INFO, "Changed Setting 'convertchar' from %u to %u", g_bCharsetConv, *(bool*) settingValue);
    g_bCharsetConv = *(bool*) settingValue;
  }
  else if (str == "timeout")
  {
    XBMC->Log(LOG_INFO, "Changed Setting 'timeout' from %u to %u", g_iConnectTimeout, *(int*) settingValue);
    g_iConnectTimeout = *(int*) settingValue;
  }
  else if (str == "autochannelgroups")
  {
    XBMC->Log(LOG_INFO, "Changed Setting 'autochannelgroups' from %u to %u", g_bAutoChannelGroups, *(bool*) settingValue);
    if (g_bAutoChannelGroups != *(bool*) settingValue)
    {
      g_bAutoChannelGroups = *(bool*) settingValue;
      return ADDON_STATUS_NEED_RESTART;
    }
  }
  else if (str == "chunksize")
  {
    XBMC->Log(LOG_INFO, "Changed Setting 'chunksize' from %u to %u", g_iChunkSize, *(int*) settingValue);
    g_iChunkSize = *(int*) settingValue;
  }

  return ADDON_STATUS_OK;
}

void ADDON_Stop()
{
}

/***********************************************************
 * PVR Client AddOn specific public library functions
 ***********************************************************/

void OnSystemSleep()
{
}

void OnSystemWake()
{
  if (XBMC && !g_szWolMac.empty())
  {
    XBMC->WakeOnLan(g_szWolMac.c_str());
  }
}

void OnPowerSavingActivated()
{
}

void OnPowerSavingDeactivated()
{
}

PVR_ERROR GetAddonCapabilities(PVR_ADDON_CAPABILITIES* pCapabilities)
{
  pCapabilities->bSupportsEPG                = true;
  pCapabilities->bSupportsRecordings         = true;
  pCapabilities->bSupportsRecordingEdl       = true;
  pCapabilities->bSupportsTimers             = true;
  pCapabilities->bSupportsTV                 = true;
  pCapabilities->bSupportsRadio              = true;
  pCapabilities->bSupportsChannelGroups      = true;
  pCapabilities->bHandlesInputStream         = true;
  pCapabilities->bHandlesDemuxing            = true;
  if (VNSIData && VNSIData->SupportChannelScan())
    pCapabilities->bSupportsChannelScan      = true;
  if (VNSIData && VNSIData->SupportRecordingsUndelete())
    pCapabilities->bSupportsRecordingsUndelete = true;
  pCapabilities->bSupportsRecordingsRename = true;
  pCapabilities->bSupportsRecordingsLifetimeChange = false;
  pCapabilities->bSupportsDescrambleInfo = false;

  return PVR_ERROR_NO_ERROR;
}

const char * GetBackendName(void)
{
  static std::string BackendName = VNSIData ? VNSIData->GetServerName() : "unknown";
  return BackendName.c_str();
}

const char * GetBackendVersion(void)
{
  static std::string BackendVersion;
  if (VNSIData) {
    std::stringstream format;
    format << VNSIData->GetVersion() << "(Protocol: " << VNSIData->GetProtocol() << ")";
    BackendVersion = format.str();
  }
  return BackendVersion.c_str();
}

const char * GetConnectionString(void)
{
  static std::string ConnectionString;
  std::stringstream format;

  if (VNSIData) {
    format << g_szHostname << ":" << g_iPort;
  }
  else {
    format << g_szHostname << ":" << g_iPort << " (addon error!)";
  }
  ConnectionString = format.str();
  return ConnectionString.c_str();
}

const char * GetBackendHostname(void)
{
  return g_szHostname.c_str();
}

PVR_ERROR GetDriveSpace(long long *iTotal, long long *iUsed)
{
  if (!VNSIData)
    return PVR_ERROR_SERVER_ERROR;

  return (VNSIData->GetDriveSpace(iTotal, iUsed) ? PVR_ERROR_NO_ERROR : PVR_ERROR_SERVER_ERROR);
}

PVR_ERROR OpenDialogChannelScan(void)
{
  cVNSIChannelScan scanner;
  try {
    scanner.Open(g_szHostname, g_iPort);
    return PVR_ERROR_NO_ERROR;
  } catch (std::exception e) {
    XBMC->Log(LOG_ERROR, "%s - %s", __FUNCTION__, e.what());
    return PVR_ERROR_SERVER_ERROR;
  }
}

/*******************************************/
/** PVR EPG Functions                     **/

PVR_ERROR GetEPGForChannel(ADDON_HANDLE handle, int iChannelUid, time_t iStart, time_t iEnd)
{
  if (!VNSIData)
    return PVR_ERROR_SERVER_ERROR;

  try {
    return (VNSIData->GetEPGForChannel(handle, iChannelUid, iStart, iEnd) ? PVR_ERROR_NO_ERROR: PVR_ERROR_SERVER_ERROR);
  } catch (std::exception e) {
    XBMC->Log(LOG_ERROR, "%s - %s", __FUNCTION__, e.what());
    return PVR_ERROR_SERVER_ERROR;
  }
}


/*******************************************/
/** PVR Channel Functions                 **/

int GetChannelsAmount(void)
{
  if (!VNSIData)
    return 0;

  try {
    return VNSIData->GetChannelsCount();
  } catch (std::exception e) {
    XBMC->Log(LOG_ERROR, "%s - %s", __FUNCTION__, e.what());
    return 0;
  }
}

PVR_ERROR GetChannels(ADDON_HANDLE handle, bool bRadio)
{
  if (!VNSIData)
    return PVR_ERROR_SERVER_ERROR;

  try {
    return (VNSIData->GetChannelsList(handle, bRadio) ? PVR_ERROR_NO_ERROR : PVR_ERROR_SERVER_ERROR);
  } catch (std::exception e) {
    XBMC->Log(LOG_ERROR, "%s - %s", __FUNCTION__, e.what());
    return PVR_ERROR_SERVER_ERROR;
  }
}

/*******************************************/
/** PVR Channelgroups Functions           **/

int GetChannelGroupsAmount()
{
  if (!VNSIData)
    return PVR_ERROR_SERVER_ERROR;

  try {
    return VNSIData->GetChannelGroupCount(g_bAutoChannelGroups);
  } catch (std::exception e) {
    XBMC->Log(LOG_ERROR, "%s - %s", __FUNCTION__, e.what());
    return PVR_ERROR_SERVER_ERROR;
  }
}

PVR_ERROR GetChannelGroups(ADDON_HANDLE handle, bool bRadio)
{
  if (!VNSIData)
    return PVR_ERROR_SERVER_ERROR;

  try {
    if(VNSIData->GetChannelGroupCount(g_bAutoChannelGroups) > 0)
      return VNSIData->GetChannelGroupList(handle, bRadio) ? PVR_ERROR_NO_ERROR : PVR_ERROR_SERVER_ERROR;

    return PVR_ERROR_NO_ERROR;
  } catch (std::exception e) {
    XBMC->Log(LOG_ERROR, "%s - %s", __FUNCTION__, e.what());
    return PVR_ERROR_SERVER_ERROR;
  }
}

PVR_ERROR GetChannelGroupMembers(ADDON_HANDLE handle, const PVR_CHANNEL_GROUP &group)
{
  if (!VNSIData)
    return PVR_ERROR_SERVER_ERROR;

  try {
    return VNSIData->GetChannelGroupMembers(handle, group) ? PVR_ERROR_NO_ERROR : PVR_ERROR_SERVER_ERROR;
  } catch (std::exception e) {
    XBMC->Log(LOG_ERROR, "%s - %s", __FUNCTION__, e.what());
    return PVR_ERROR_SERVER_ERROR;
  }
}

/*******************************************/
/** PVR Timer Functions                   **/


PVR_ERROR GetTimerTypes(PVR_TIMER_TYPE types[], int *size)
{
  if (!VNSIData)
    return PVR_ERROR_SERVER_ERROR;

  try
  {
    return VNSIData->GetTimerTypes(types, size);
  }
  catch (std::exception e)
  {
    XBMC->Log(LOG_ERROR, "%s - %s", __FUNCTION__, e.what());
    return PVR_ERROR_SERVER_ERROR;
  }
}

int GetTimersAmount(void)
{
  if (!VNSIData)
    return PVR_ERROR_SERVER_ERROR;

  try
  {
    return VNSIData->GetTimersCount();
  }
  catch (std::exception e)
  {
    XBMC->Log(LOG_ERROR, "%s - %s", __FUNCTION__, e.what());
    return PVR_ERROR_SERVER_ERROR;
  }
}

PVR_ERROR GetTimers(ADDON_HANDLE handle)
{
  if (!VNSIData)
    return PVR_ERROR_SERVER_ERROR;

  try
  {
    return (VNSIData->GetTimersList(handle) ? PVR_ERROR_NO_ERROR : PVR_ERROR_SERVER_ERROR);
  }
  catch (std::exception e)
  {
    XBMC->Log(LOG_ERROR, "%s - %s", __FUNCTION__, e.what());
    return PVR_ERROR_SERVER_ERROR;
  }
}

PVR_ERROR AddTimer(const PVR_TIMER &timer)
{
  if (!VNSIData)
    return PVR_ERROR_SERVER_ERROR;

  try
  {
    return VNSIData->AddTimer(timer);
  }
  catch (std::exception e)
  {
    XBMC->Log(LOG_ERROR, "%s - %s", __FUNCTION__, e.what());
    return PVR_ERROR_SERVER_ERROR;
  }
}

PVR_ERROR DeleteTimer(const PVR_TIMER &timer, bool bForce)
{
  if (!VNSIData)
    return PVR_ERROR_SERVER_ERROR;

  try
  {
    return VNSIData->DeleteTimer(timer, bForce);
  }
  catch (std::exception e)
  {
    XBMC->Log(LOG_ERROR, "%s - %s", __FUNCTION__, e.what());
    return PVR_ERROR_SERVER_ERROR;
  }
}

PVR_ERROR UpdateTimer(const PVR_TIMER &timer)
{
  if (!VNSIData)
    return PVR_ERROR_SERVER_ERROR;

  try
  {
    return VNSIData->UpdateTimer(timer);
  }
  catch (std::exception e)
  {
    XBMC->Log(LOG_ERROR, "%s - %s", __FUNCTION__, e.what());
    return PVR_ERROR_SERVER_ERROR;
  }
}


/*******************************************/
/** PVR Recording Functions               **/

int GetRecordingsAmount(bool deleted)
{
  if (!VNSIData)
    return 0;

  try {
    if (!deleted)
      return VNSIData->GetRecordingsCount();
    else
      return VNSIData->GetDeletedRecordingsCount();
  } catch (std::exception e) {
    XBMC->Log(LOG_ERROR, "%s - %s", __FUNCTION__, e.what());
    return PVR_ERROR_SERVER_ERROR;
  }
}

PVR_ERROR GetRecordings(ADDON_HANDLE handle, bool deleted)
{
  if (!VNSIData)
    return PVR_ERROR_SERVER_ERROR;

  try {
    if (!deleted)
      return VNSIData->GetRecordingsList(handle);
    else
      return VNSIData->GetDeletedRecordingsList(handle);
  } catch (std::exception e) {
    XBMC->Log(LOG_ERROR, "%s - %s", __FUNCTION__, e.what());
    return PVR_ERROR_SERVER_ERROR;
  }
}

PVR_ERROR RenameRecording(const PVR_RECORDING &recording)
{
  if (!VNSIData)
    return PVR_ERROR_SERVER_ERROR;

  try {
    return VNSIData->RenameRecording(recording, recording.strTitle);
  } catch (std::exception e) {
    XBMC->Log(LOG_ERROR, "%s - %s", __FUNCTION__, e.what());
    return PVR_ERROR_SERVER_ERROR;
  }
}

PVR_ERROR DeleteRecording(const PVR_RECORDING &recording)
{
  if (!VNSIData)
    return PVR_ERROR_SERVER_ERROR;

  try {
    return VNSIData->DeleteRecording(recording);
  } catch (std::exception e) {
    XBMC->Log(LOG_ERROR, "%s - %s", __FUNCTION__, e.what());
    return PVR_ERROR_SERVER_ERROR;
  }
}

PVR_ERROR UndeleteRecording(const PVR_RECORDING& recording)
{
  if (!VNSIData)
    return PVR_ERROR_SERVER_ERROR;

  try {
    return VNSIData->UndeleteRecording(recording);
  } catch (std::exception e) {
    XBMC->Log(LOG_ERROR, "%s - %s", __FUNCTION__, e.what());
    return PVR_ERROR_SERVER_ERROR;
  }
}

PVR_ERROR DeleteAllRecordingsFromTrash()
{
  if (!VNSIData)
    return PVR_ERROR_SERVER_ERROR;

  try {
    return VNSIData->DeleteAllRecordingsFromTrash();
  } catch (std::exception e) {
    XBMC->Log(LOG_ERROR, "%s - %s", __FUNCTION__, e.what());
    return PVR_ERROR_SERVER_ERROR;
  }
}

/*******************************************/
/** PVR Live Stream Functions             **/

bool OpenLiveStream(const PVR_CHANNEL &channel)
{
  CloseLiveStream();

  try
  {
    VNSIDemuxer = new cVNSIDemux;
    IsRealtime = true;
    if (!VNSIDemuxer->OpenChannel(channel)) {
      delete VNSIDemuxer;
      VNSIDemuxer = nullptr;
      return false;
    }

    return true;
  }
  catch (std::exception e)
  {
    XBMC->Log(LOG_ERROR, "%s - %s", __FUNCTION__, e.what());
    delete VNSIDemuxer;
    VNSIDemuxer = NULL;
    return false;
  }

}

void CloseLiveStream(void)
{
  delete VNSIDemuxer;
  VNSIDemuxer = NULL;
}

PVR_ERROR GetStreamProperties(PVR_STREAM_PROPERTIES* pProperties)
{
  if (!VNSIDemuxer)
    return PVR_ERROR_SERVER_ERROR;

  return (VNSIDemuxer->GetStreamProperties(pProperties) ? PVR_ERROR_NO_ERROR : PVR_ERROR_SERVER_ERROR);
}

void DemuxAbort(void)
{
  if (VNSIDemuxer)
    VNSIDemuxer->Abort();
}

DemuxPacket* DemuxRead(void)
{
  if (!VNSIDemuxer)
    return NULL;

  DemuxPacket *pkt;
  try {
    pkt = VNSIDemuxer->Read();
  } catch (std::exception e) {
    XBMC->Log(LOG_ERROR, "%s - %s", __FUNCTION__, e.what());
    return NULL;
  }

  if (pkt)
  {
    const CLockObject lock(TimeshiftMutex);
    IsTimeshift = VNSIDemuxer->IsTimeshift();
    if ((PTSBufferEnd - pkt->dts) / DVD_TIME_BASE > 10)
      IsRealtime = false;
    else
      IsRealtime = true;
  }
  return pkt;
}

PVR_ERROR GetStreamTimes(PVR_STREAM_TIMES *times)
{
  if (VNSIDemuxer && VNSIDemuxer->GetStreamTimes(times))
  {
    PTSBufferEnd = times->ptsEnd;
    return PVR_ERROR_NO_ERROR;
  }
  else if (VNSIRecording && VNSIRecording->GetStreamTimes(times))
  {
    PTSBufferEnd = times->ptsEnd;
    return PVR_ERROR_NO_ERROR;
  }
  else
    return PVR_ERROR_SERVER_ERROR;
}

PVR_ERROR SignalStatus(PVR_SIGNAL_STATUS &signalStatus)
{
  if (!VNSIDemuxer)
    return PVR_ERROR_SERVER_ERROR;

  try {
    return (VNSIDemuxer->GetSignalStatus(signalStatus) ? PVR_ERROR_NO_ERROR : PVR_ERROR_SERVER_ERROR);
  } catch (std::exception e) {
    XBMC->Log(LOG_ERROR, "%s - %s", __FUNCTION__, e.what());
    return PVR_ERROR_SERVER_ERROR;
  }
}

bool CanPauseStream(void)
{
  bool ret = false;
  if (VNSIDemuxer)
    ret = VNSIDemuxer->IsTimeshift();
  return ret;
}

bool CanSeekStream(void)
{
  bool ret = false;
  if (VNSIDemuxer)
    ret = VNSIDemuxer->IsTimeshift();
  return ret;
}

bool IsRealTimeStream()
{
  if (VNSIDemuxer)
  {
    const CLockObject lock(TimeshiftMutex);
    if (!IsTimeshift)
      return true;
    if (IsRealtime)
      return true;
  }
  return false;
}

bool SeekTime(double time, bool backwards, double *startpts)
{
  bool ret = false;
  try
  {
    if (VNSIDemuxer)
      ret = VNSIDemuxer->SeekTime(time, backwards, startpts);
  } catch (std::exception e) {
    XBMC->Log(LOG_ERROR, "%s - %s", __FUNCTION__, e.what());
  }
  return ret;
}

void SetSpeed(int) {};
void PauseStream(bool bPaused) {}

/*******************************************/
/** PVR Recording Stream Functions        **/

bool OpenRecordedStream(const PVR_RECORDING &recording)
{
  if(!VNSIData)
    return false;

  CloseRecordedStream();

  VNSIRecording = new cVNSIRecording;
  try
  {
    if (!VNSIRecording->OpenRecording(recording)) {
      delete VNSIRecording;
      VNSIRecording = nullptr;
      return false;
    }

    return true;
  }
  catch (std::exception e)
  {
    XBMC->Log(LOG_ERROR, "%s - %s", __FUNCTION__, e.what());
    delete VNSIRecording;
    VNSIRecording = NULL;
    return false;
  }
}

void CloseRecordedStream(void)
{
  delete VNSIRecording;
  VNSIRecording = NULL;
}

int ReadRecordedStream(unsigned char *pBuffer, unsigned int iBufferSize)
{
  if (!VNSIRecording)
    return -1;

  try {
    return VNSIRecording->Read(pBuffer, iBufferSize);
  } catch (std::exception e) {
    XBMC->Log(LOG_ERROR, "%s - %s", __FUNCTION__, e.what());
    return -1;
  }
}

long long SeekRecordedStream(long long iPosition, int iWhence /* = SEEK_SET */)
{
  try {
    if (VNSIRecording)
      return VNSIRecording->Seek(iPosition, iWhence);
  } catch (std::exception e) {
    XBMC->Log(LOG_ERROR, "%s - %s", __FUNCTION__, e.what());
  }

  return -1;
}

long long LengthRecordedStream(void)
{
  if (VNSIRecording)
    return VNSIRecording->Length();

  return 0;
}

PVR_ERROR GetRecordingEdl(const PVR_RECORDING& recinfo, PVR_EDL_ENTRY edl[], int *size)
{
  if(!VNSIData)
    return PVR_ERROR_UNKNOWN;

  try {
    return VNSIData->GetRecordingEdl(recinfo, edl, size);
  } catch (std::exception e) {
    XBMC->Log(LOG_ERROR, "%s - %s", __FUNCTION__, e.what());
    return PVR_ERROR_SERVER_ERROR;
  }
}

PVR_ERROR GetStreamReadChunkSize(int* chunksize)
{
  *chunksize = g_iChunkSize;
  return PVR_ERROR_NO_ERROR;
}

/*******************************************/
/** PVR Menu Hook Functions               **/

PVR_ERROR CallMenuHook(const PVR_MENUHOOK &menuhook, const PVR_MENUHOOK_DATA &item)
{
  try {
    if (menuhook.iHookId == 1)
    {
      cVNSIAdmin osd;
      osd.Open(g_szHostname, g_iPort);
    }
    return PVR_ERROR_NO_ERROR;
  } catch (std::exception e) {
    XBMC->Log(LOG_ERROR, "%s - %s", __FUNCTION__, e.what());
    return PVR_ERROR_SERVER_ERROR;
  }
}

/** UNUSED API FUNCTIONS */
PVR_ERROR DeleteChannel(const PVR_CHANNEL &channel) { return PVR_ERROR_NOT_IMPLEMENTED; }
PVR_ERROR RenameChannel(const PVR_CHANNEL &channel) { return PVR_ERROR_NOT_IMPLEMENTED; }
PVR_ERROR OpenDialogChannelSettings(const PVR_CHANNEL &channel) { return PVR_ERROR_NOT_IMPLEMENTED; }
PVR_ERROR OpenDialogChannelAdd(const PVR_CHANNEL &channel) { return PVR_ERROR_NOT_IMPLEMENTED; }
void DemuxReset(void) {}
void DemuxFlush(void) {}
void FillBuffer(bool mode) {}
int ReadLiveStream(unsigned char *pBuffer, unsigned int iBufferSize) { return 0; }
long long SeekLiveStream(long long iPosition, int iWhence /* = SEEK_SET */) { return -1; }
long long LengthLiveStream(void) { return -1; }
PVR_ERROR SetRecordingPlayCount(const PVR_RECORDING &recording, int count) { return PVR_ERROR_NOT_IMPLEMENTED; }
PVR_ERROR SetRecordingLastPlayedPosition(const PVR_RECORDING &recording, int lastplayedposition) { return PVR_ERROR_NOT_IMPLEMENTED; }
int GetRecordingLastPlayedPosition(const PVR_RECORDING &recording) { return -1; }
PVR_ERROR SetEPGTimeFrame(int) { return PVR_ERROR_NOT_IMPLEMENTED; }
PVR_ERROR GetDescrambleInfo(PVR_DESCRAMBLE_INFO*) { return PVR_ERROR_NOT_IMPLEMENTED; }
PVR_ERROR SetRecordingLifetime(const PVR_RECORDING*) { return PVR_ERROR_NOT_IMPLEMENTED; }
PVR_ERROR GetChannelStreamProperties(const PVR_CHANNEL*, PVR_NAMED_VALUE*, unsigned int*) { return PVR_ERROR_NOT_IMPLEMENTED; }
PVR_ERROR GetRecordingStreamProperties(const PVR_RECORDING*, PVR_NAMED_VALUE*, unsigned int*) { return PVR_ERROR_NOT_IMPLEMENTED; }
PVR_ERROR IsEPGTagRecordable(const EPG_TAG*, bool*) { return PVR_ERROR_NOT_IMPLEMENTED; }
PVR_ERROR IsEPGTagPlayable(const EPG_TAG*, bool*) { return PVR_ERROR_NOT_IMPLEMENTED; }
PVR_ERROR GetEPGTagStreamProperties(const EPG_TAG*, PVR_NAMED_VALUE*, unsigned int*) { return PVR_ERROR_NOT_IMPLEMENTED; }
PVR_ERROR GetEPGTagEdl(const EPG_TAG* epgTag, PVR_EDL_ENTRY edl[], int *size) { return PVR_ERROR_NOT_IMPLEMENTED; }

}
