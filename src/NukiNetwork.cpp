#include "NukiNetwork.h"
#include "PreferencesKeys.h"
#include "Logger.h"
#include "Config.h"
#include "RestartReason.h"
#include "hal/wdt_hal.h"

NukiNetwork *NukiNetwork::_inst = nullptr;

extern bool ethCriticalFailure;
extern bool wifiFallback;
extern bool disableNetwork;
extern bool forceEnableWebServer;

NukiNetwork::NukiNetwork(Preferences *preferences, char *buffer, size_t bufferSize)
    : _preferences(preferences),
      _buffer(buffer),
      _bufferSize(bufferSize)
{

  _inst = this;
  _webEnabled = _preferences->getBool(preference_webcfgserver_enabled, true);
  _apitoken = new BridgeApiToken(_preferences, preference_api_Token);
  _apiEnabled = _preferences->getBool(preference_api_enabled);
  setupDevice();
}

void NukiNetwork::setupDevice()
{

  _ipConfiguration = new IPConfiguration(_preferences);

  _firstBootAfterDeviceChange = _preferences->getBool(preference_ntw_reconfigure, false);

  if (wifiFallback == true)
  {
    if (!_firstBootAfterDeviceChange)
    {
      Log->println(F("[ERROR] Failed to connect to network. Wi-Fi fallback is disabled, rebooting."));
      wifiFallback = false;
      sleep(5);
      restartEsp(RestartReason::NetworkDeviceCriticalFailureNoWifiFallback);
    }

    Log->println(F("[INFO] Switching to Wi-Fi device as fallback."));
    _networkDeviceType = NetworkDeviceType::WiFi;
  }
  else
  {
    _networkDeviceType = NetworkDeviceType::ETH;
  }
}

// WiFi stuff
// ###############################################################
void NukiNetwork::reconfigure()
{

  switch (_networkDeviceType)
  {
  case NetworkDeviceType::WiFi:
    _preferences->putString(preference_wifi_ssid, "");
    _preferences->putString(preference_wifi_pass, "");
    delay(200);
    restartEsp(RestartReason::ReconfigureWifi);
    break;
  case NetworkDeviceType::ETH:
    delay(200);
    restartEsp(RestartReason::ReconfigureETH);
    break;
  }
}

void NukiNetwork::scan(bool passive, bool async)
{
  if (_networkDeviceType == NetworkDeviceType::WiFi)
  {
    if (!_openAP)
    {
      WiFi.disconnect(true);
      WiFi.mode(WIFI_STA);
      WiFi.disconnect();
    }

    WiFi.scanDelete();
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

    if (async)
    {
      Log->println(F("[INFO] Wi-Fi async scan started"));
    }
    else
    {
      Log->println(F("[INFO] Wi-Fi sync scan started"));
    }
    if (passive)
    {
      WiFi.scanNetworks(async, false, true, 75U);
    }
    else
    {
      WiFi.scanNetworks(async);
    }
  }
}

bool NukiNetwork::isApOpen() const
{

  return (_networkDeviceType == NetworkDeviceType::WiFi ? _openAP : false);
}

const String NukiNetwork::networkBSSID() const
{
  return (_networkDeviceType == NetworkDeviceType::WiFi ? WiFi.BSSIDstr() : String(""));
}

NetworkDeviceType NukiNetwork::networkDeviceType()
{
  return _networkDeviceType;
}

void NukiNetwork::clearWifiFallback()
{
  wifiFallback = false;
}

bool NukiNetwork::isConnected() const
{
  return (_networkDeviceType == NetworkDeviceType::WiFi ? WiFi.isConnected() : _connected);
}

bool NukiNetwork::isWifiConnected()
{
  return (_networkDeviceType != NetworkDeviceType::WiFi ? true : isConnected());
}

bool NukiNetwork::isWifiConfigured() const
{
  return _WiFissid.length() > 0 && _WiFipass.length() > 0;
}

void NukiNetwork::initialize()
{

  if (!disableNetwork)
  {

    strncpy(_apiBridgePath, api_path_bridge, sizeof(_apiBridgePath) - 1);

    _homeAutomationEnabled = _preferences->getBool(preference_ha_enabled, false);
    _homeAutomationAdress = _preferences->getString(preference_ha_address, "");

    _hostname = _preferences->getString(preference_hostname, "");

    if (_hostname == "")
    {
      _hostname = "nukibridge";
      _preferences->putString(preference_hostname, _hostname);
    }

    _homeAutomationPort = _preferences->getInt(preference_ha_port, 0);

    if (_homeAutomationPort == 0)
    {
      _homeAutomationPort = 80;
      _preferences->putInt(preference_ha_port, _homeAutomationPort);
    }

    switch (_networkDeviceType)
    {
    case NetworkDeviceType::WiFi:
      initializeWiFi();
      break;
    case NetworkDeviceType::ETH:
      initializeEthernet();
      break;
    }

    Log->print("Host name: ");
    Log->println(_hostname);

    String _homeAutomationUser = _preferences->getString(preference_ha_user);

    String _homeAutomationPass = _preferences->getString(preference_ha_password);

    readSettings();

    startNetworkServices();
  }
}

void NukiNetwork::startNetworkServices()
{

  _httpClient = new HTTPClient();
  _server = new WebServer(REST_SERVER_PORT);
  if (_server)
  {
    _server->onNotFound([this]()
                        { onRestDataReceivedCallback(this->_server->uri().c_str(), *this->_server); });
    _server->begin();
  }
}

void NukiNetwork::restartNetworkServices(int status)
{
  if (status == 1)
  {
    status = testNetworkServices();
  }

  if (status == 0)
  {
    Log->println(F("[OK] Network services are running."));
    return; // No restart required
  }

  // If _httpClient is not reachable (-2 or -3), reinitialize
  if (status == -2 || status == -3)
  {
    Log->println(F("[INFO] Reinitialization of HTTP client..."));
    if (_httpClient)
    {
      delete _httpClient;
      _httpClient = nullptr;
    }
    _httpClient = new HTTPClient();
    if (_httpClient)
    {
      Log->println(F("[OK] HTTP client successfully reinitialized."));
    }
    else
    {
      Log->println(F("[ERROR] HTTP client cannot be initialized."));
    }
  }

  // If the REST web server cannot be reached (-1 or -3), restart it
  if (status == -1 || status == -3)
  {
    Log->println(F("[INFO] Restarting the REST WebServer..."));
    if (_server)
    {
      _server->stop();
      delete _server;
      _server = nullptr;
    }
    _server = new WebServer(REST_SERVER_PORT);
    if (_server)
    {
      _server->onNotFound([this]()
                          { onRestDataReceivedCallback(this->_server->uri().c_str(), *this->_server); });
      _server->begin();
      Log->println(F("[OK] REST WebServer successfully restarted."));
    }
    else
    {
      Log->println(F("[ERROR] REST Web Server cannot be initialized."));
    }
  }

  Log->println(F("[INFO] Network services have been checked and reinit/restarted if necessary."));
}

int NukiNetwork::testNetworkServices()
{
  bool httpClientOk = true;
  bool webServerOk = true;

  // 1. check whether _httpClient exists
  if (_httpClient == nullptr)
  {
    Log->println(F("[ERROR] _httpClient is NULL!"));
    httpClientOk = false;
  }
  if (_homeAutomationEnabled)
  {
    // 2. ping test for _homeAutomationAdress
    if (!_homeAutomationAdress.isEmpty() && httpClientOk)
    {
      if (!Ping.ping(_homeAutomationAdress.c_str()))
      {
        Log->println(F("[ERROR] Ping to Home Automation Server failed!"));
        httpClientOk = false;
      }
      else
      {
        Log->println(F("[OK] Ping to Home Automation Server successful."));
      }
    }

    // 3. if Home Automation API path exists, execute GET request
    String strPath = _preferences->getString(preference_ha_path_state, "");
    if (!strPath.isEmpty() && httpClientOk)
    {
      String url = "http://" + _homeAutomationAdress + ":" + String(_homeAutomationPort) + "/" + strPath;
      Log->println("[INFO] Performing GET request to: " + url);

      HTTPClient http;
      http.begin(url);
      int httpCode = http.GET();
      http.end();

      if (httpCode > 0)
      {
        Log->println("[OK] HTTP GET successful, response code: " + String(httpCode));
      }
      else
      {
        Log->println("[ERROR] HTTP GET failed!");
        httpClientOk = false;
      }
    }
  }

  // 4. check whether Rest Server (_server) exists
  if (_server == nullptr)
  {
    Log->println(F("[ERROR] _server is NULL!"));
    webServerOk = false;
  }

  // 5. test whether the local REST web server can be reached on the port
  WiFiClient client;
  if (!client.connect(WiFi.localIP(), REST_SERVER_PORT))
  {
    Log->println("[ERROR] WebServer is not responding!");
    webServerOk = false;
  }
  else
  {
    Log->println("[OK] WebServer is responding.");
    client.stop();
  }

  // 6. return error code
  if (webServerOk && httpClientOk)
    return 0; // all OK
  if (!webServerOk && httpClientOk)
    return -1; // _server not reachable
  if (webServerOk && !httpClientOk)
    return -2; // _httpClient / Home Automation not reachable
  return -3;   // Both _server and _httpClient not reachable
}

void NukiNetwork::readSettings()
{
  _disableNetworkIfNotConnected = _preferences->getBool(preference_disable_network_not_connected, false);
  _restartOnDisconnect = _preferences->getBool(preference_restart_on_disconnect, false);
  _rssiPublishInterval = _preferences->getInt(preference_rssi_publish_interval, 0) * 1000;
  _MaintenancePublishIntervall = _preferences->getInt(preference_Maintenance_publish_interval, 0) * 1000;

  if (_rssiPublishInterval == 0)
  {
    _rssiPublishInterval = 60000;
    _preferences->putInt(preference_rssi_publish_interval, 60);
  }

  _networkTimeout = _preferences->getInt(preference_network_timeout, 0);
  if (_networkTimeout == 0)
  {
    _networkTimeout = -1;
    _preferences->putInt(preference_network_timeout, _networkTimeout);
  }
}

void NukiNetwork::initializeWiFi()
{

  _WiFissid = _preferences->getString(preference_wifi_ssid, "");
  _WiFissid.trim();
  _WiFipass = _preferences->getString(preference_wifi_pass, "");
  _WiFipass.trim();
  WiFi.setHostname(_hostname.c_str());

  WiFi.onEvent([this](arduino_event_id_t event, arduino_event_info_t info)
               { this->onNetworkEvent(event, info); });

  if (isWifiConfigured())
  {
    Log->println(String("[INFO] Attempting to connect to saved SSID ") + String(_WiFissid));
    _openAP = false;
  }
  else
  {
    Log->println("[INFO] No SSID or Wifi password saved, opening AP");
    _openAP = true;
  }

  scan(false, true);
  return;
}

void NukiNetwork::initializeEthernet()
{

  delay(250);
  if (ethCriticalFailure)
  {
    ethCriticalFailure = false;
    Log->println(F("[ERROR] Failed to initialize ethernet hardware"));
    Log->println(F("[ERROR] Network device has a critical failure, enable fallback to Wi-Fi and reboot."));
    wifiFallback = true;
    delay(200);
    restartEsp(RestartReason::NetworkDeviceCriticalFailure);
    return;
  }

  Log->println("[INFO] Init Ethernet");

  ethCriticalFailure = true;
  _hardwareInitialized = ETH.begin();
  ethCriticalFailure = false;

  if (_hardwareInitialized)
  {
    Log->println("[OK] Ethernet hardware Initialized");
    wifiFallback = false;

    if (!_ipConfiguration->dhcpEnabled())
    {
      ETH.config(_ipConfiguration->ipAddress(), _ipConfiguration->defaultGateway(), _ipConfiguration->subnet(), _ipConfiguration->dnsServer());
    }

    WiFi.onEvent([this](arduino_event_id_t event, arduino_event_info_t info)
                 { this->onNetworkEvent(event, info); });
  }
  else
  {
    Log->println(F("[ERROR] Failed to initialize ethernet hardware"));
    Log->println(F("[ERROR] Network device has a critical failure, enable fallback to Wi-Fi and reboot."));
    wifiFallback = true;
    delay(200);
    restartEsp(RestartReason::NetworkDeviceCriticalFailure);
    return;
  }
}

bool NukiNetwork::update()
{
  wdt_hal_context_t rtc_wdt_ctx = RWDT_HAL_CONTEXT_DEFAULT();
  wdt_hal_write_protect_disable(&rtc_wdt_ctx);
  wdt_hal_feed(&rtc_wdt_ctx);
  wdt_hal_write_protect_enable(&rtc_wdt_ctx);
  int64_t ts = espMillis();

  // update device
  switch (_networkDeviceType)
  {
  case NetworkDeviceType::WiFi:
    break;
  case NetworkDeviceType::ETH:
    if (_checkIpTs != -1 && _checkIpTs < espMillis())
    {
      if (_ipConfiguration->ipAddress() != ETH.localIP())
      {
        Log->println("[INFO] ETH Set static IP");
        ETH.config(_ipConfiguration->ipAddress(), _ipConfiguration->defaultGateway(), _ipConfiguration->subnet(), _ipConfiguration->dnsServer());
        _checkIpTs = espMillis() + 5000;
      }

      _checkIpTs = -1;
    }
    break;
  }

  if (disableNetwork || isApOpen())
  {
    return false;
  }

  if (!isConnected() || (_NetworkServicesConnectCounter > 15))
  {
    _NetworkServicesConnectCounter = 0;

    if (_restartOnDisconnect && espMillis() > 60000)
    {
      restartEsp(RestartReason::RestartOnDisconnectWatchdog);
    }
    else if (_disableNetworkIfNotConnected && espMillis() > 60000)
    {
      disableNetwork = true;
      restartEsp(RestartReason::DisableNetworkIfNotConnected);
    }
  }

  if (isConnected())
  {
    if (ts - _lastNetworkServiceTs > 30000)
    { // test all 30 seconds
      _lastNetworkServiceTs = ts;
      _networkServicesState = testNetworkServices();
      if (_networkServicesState)
      { // error in networ Services
        restartNetworkServices(_networkServicesState);
        delay(1000);
        _networkServicesState = testNetworkServices(); // test network services again
        if (_networkServicesState)
        {
          _NetworkServicesConnectCounter++;
          return false;
        }
      }
    }

    _NetworkServicesConnectCounter = 0;
    if (forceEnableWebServer && !_webEnabled)
    {
      forceEnableWebServer = false;
      delay(200);
      restartEsp(RestartReason::ReconfigureWebServer);
    }
    else if (!_webEnabled)
    {
      forceEnableWebServer = false;
    }
    delay(2000);
  }

  if (!isConnected())
  {
    if (_networkTimeout > 0 && (ts - _lastConnectedTs > _networkTimeout * 1000) && ts > 60000)
    {
      if (!_webEnabled)
      {
        forceEnableWebServer = true;
      }
      Log->println(F("[WARNING] Network timeout has been reached, restarting ..."));
      delay(200);
      restartEsp(RestartReason::NetworkTimeoutWatchdog);
    }
    delay(2000);
    return false;
  }

  _lastConnectedTs = ts;

  if (_homeAutomationEnabled && (signalStrength() != 127 && _rssiPublishInterval > 0 && ts - _lastRssiTs > _rssiPublishInterval))
  {
    _lastRssiTs = ts;
    int8_t rssi = signalStrength();

    if (rssi != _lastRssi)
    {
      sendToHAInt(preference_ha_path_wifi_rssi, preference_ha_query_wifi_rssi, signalStrength());
      _lastRssi = rssi;
    }
  }

  if (_homeAutomationEnabled && (_lastMaintenanceTs == 0 || (ts - _lastMaintenanceTs) > _MaintenancePublishIntervall))
  {
    int64_t curUptime = ts / 1000 / 60;
    if (curUptime > _publishedUpTime)
    {
      sendToHAULong(preference_ha_path_uptime, preference_ha_query_uptime, curUptime);
      _publishedUpTime = curUptime;
    }

    if (_lastMaintenanceTs == 0)
    {
      sendToHAString(preference_ha_path_restart_reason_fw, preference_ha_query_restart_reason_fw, getRestartReason().c_str());
      sendToHAString(preference_ha_path_restart_reason_esp, preference_ha_query_restart_reason_esp, getEspRestartReason().c_str());
      sendToHAString(preference_ha_path_info_nuki_bridge_version, preference_ha_query_info_nuki_bridge_version, NUKI_REST_BRIDGE_VERSION);
      sendToHAString(preference_ha_path_info_nuki_bridge_build, preference_ha_query_info_nuki_bridge_build, NUKI_REST_BRIDGE_BUILD);
    }
    if (_publishDebugInfo)
    {
      sendToHAUInt(preference_ha_path_freeheap, preference_ha_query_freeheap, esp_get_free_heap_size());
    }
    _lastMaintenanceTs = ts;
  }

  return true;
}

void NukiNetwork::onNetworkEvent(arduino_event_id_t event, arduino_event_info_t info)
{
  Log->printf("[LAN Event] event: %d\n", event);

  switch (event)
  {
  // --- Ethernet Events ---
  case ARDUINO_EVENT_ETH_START:
    Log->println("[INFO] ETH Started");
    ETH.setHostname(_hostname.c_str());
    break;

  case ARDUINO_EVENT_ETH_CONNECTED:
    Log->println(F("[OK] ETH Connected"));
    if (!localIP().equals("0.0.0.0"))
    {
      _connected = true;
    }
    break;

  case ARDUINO_EVENT_ETH_GOT_IP:
    Log->printf("[INFO] ETH Got IP: '%s'\n", esp_netif_get_desc(info.got_ip.esp_netif));
    Log->println(ETH.localIP().toString());

    _connected = true;
    if (_preferences->getBool(preference_ntw_reconfigure, false))
    {
      _preferences->putBool(preference_ntw_reconfigure, false);
    }
    break;

  case ARDUINO_EVENT_ETH_LOST_IP:
    Log->println("[WARNING] ETH Lost IP");
    _connected = false;
    onDisconnected();
    break;

  case ARDUINO_EVENT_ETH_DISCONNECTED:
    Log->println("[WARNING] ETH Disconnected");
    _connected = false;
    onDisconnected();
    break;

  case ARDUINO_EVENT_ETH_STOP:
    Log->println("[WARNING] ETH Stopped");
    _connected = false;
    onDisconnected();
    break;

  // --- WiFi Events ---
  case ARDUINO_EVENT_WIFI_READY:
    Log->println("[OK] WiFi interface ready");
    break;

  case ARDUINO_EVENT_WIFI_SCAN_DONE:
    Log->println("[INFO] Completed scan for access points");
    _foundNetworks = WiFi.scanComplete();

    for (int i = 0; i < _foundNetworks; i++)
    {
      Log->println(String("SSID ") + WiFi.SSID(i) + String(" found with RSSI: ") + String(WiFi.RSSI(i)) + String(("(")) + String(constrain((100.0 + WiFi.RSSI(i)) * 2, 0, 100)) + String(" %) and BSSID: ") + WiFi.BSSIDstr(i) + String(" and channel: ") + String(WiFi.channel(i)));
    }

    if (_openAP)
    {
      openAP();
    }
    else if (_foundNetworks > 0 || _preferences->getBool(preference_find_best_rssi, false))
    {
      esp_wifi_scan_stop();
      connect();
    }
    else
    {
      Log->println("[INFO] No networks found, restarting scan");
      scan(false, true);
    }
    break;

  case ARDUINO_EVENT_WIFI_STA_START:
    Log->println("[INFO] WiFi client started");
    break;

  case ARDUINO_EVENT_WIFI_STA_STOP:
    Log->println("[INFO] WiFi clients stopped");
    if (!_openAP)
    {
      onDisconnected();
    }
    break;

  case ARDUINO_EVENT_WIFI_STA_CONNECTED:
    Log->println("Connected to access point");
    if (!_openAP)
    {
      onConnected();
    }
    break;

  case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    Log->println("Disconnected from WiFi access point");
    if (!_openAP)
    {
      onDisconnected();
    }
    break;

  case ARDUINO_EVENT_WIFI_STA_AUTHMODE_CHANGE:
    Log->println("Authentication mode of access point has changed");
    break;

  case ARDUINO_EVENT_WIFI_STA_GOT_IP:
    Log->print("Obtained IP address: ");
    Log->println(WiFi.localIP());
    if (!_openAP)
    {
      onConnected();
    }
    break;

  case ARDUINO_EVENT_WIFI_STA_LOST_IP:
    Log->println("Lost IP address and IP address is reset to 0");
    if (!_openAP)
    {
      onDisconnected();
    }
    break;

  case ARDUINO_EVENT_WIFI_AP_START:
    Log->println("WiFi access point started");
    break;

  case ARDUINO_EVENT_WIFI_AP_STOP:
    Log->println("WiFi access point stopped");
    break;

  case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
    Log->println("Client connected");
    break;

  case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
    Log->println("Client disconnected");
    break;

  case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
    Log->println("Assigned IP address to client");
    break;

  case ARDUINO_EVENT_WIFI_AP_PROBEREQRECVED:
    Log->println("Received probe request");
    break;

  case ARDUINO_EVENT_WIFI_AP_GOT_IP6:
    Log->println("AP IPv6 is preferred");
    break;
  case ARDUINO_EVENT_WIFI_STA_GOT_IP6:
    Log->println("STA IPv6 is preferred");
    break;

  default:
    Log->print("Unknown LAN Event: ");
    Log->println(event);
    break;
  }
}

void NukiNetwork::onConnected()
{
  if (_networkDeviceType == NetworkDeviceType::WiFi)
  {
    Log->println("Wi-Fi connected");
    _connected = true;
  }
}

void NukiNetwork::onDisconnected()
{
  switch (_networkDeviceType)
  {
  case NetworkDeviceType::WiFi:
    if (!_connected)
    {
      return;
    }
    _connected = false;

    Log->println("[WARNING] Wi-Fi disconnected");
    connect();
    break;
  case NetworkDeviceType::ETH:
    if (_preferences->getBool(preference_restart_on_disconnect, false) && (espMillis() > 60000))
    {
      restartEsp(RestartReason::RestartOnDisconnectWatchdog);
    }
    break;
  }
}

void NukiNetwork::openAP()
{
  if (_startAP)
  {
    Log->println("Starting AP with SSID NukiBridge and Password NukiBridgeESP32");
    _startAP = false;
    WiFi.mode(WIFI_AP);
    delay(500);
    WiFi.softAPsetHostname(_hostname.c_str());
    delay(500);
    WiFi.softAP("NukiBridge", "NukiBridgeESP32");
  }
}

bool NukiNetwork::connect()
{

  if (_networkDeviceType == NetworkDeviceType::WiFi)
  {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(_hostname.c_str());
    delay(500);

    int bestConnection = -1;

    if (_preferences->getBool(preference_find_best_rssi, false))
    {
      for (int i = 0; i < _foundNetworks; i++)
      {
        if (_WiFissid == WiFi.SSID(i))
        {
          Log->println(String("Saved SSID ") + _WiFissid + String(" found with RSSI: ") + String(WiFi.RSSI(i)) + String(("(")) + String(constrain((100.0 + WiFi.RSSI(i)) * 2, 0, 100)) + String(" %) and BSSID: ") + WiFi.BSSIDstr(i) + String(" and channel: ") + String(WiFi.channel(i)));
          if (bestConnection == -1)
          {
            bestConnection = i;
          }
          else
          {
            if (WiFi.RSSI(i) > WiFi.RSSI(bestConnection))
            {
              bestConnection = i;
            }
          }
        }
      }

      if (bestConnection == -1)
      {
        Log->print("No network found with SSID: ");
        Log->println(_WiFissid);
      }
      else
      {
        Log->println(String("Trying to connect to SSID ") + _WiFissid + String(" found with RSSI: ") + String(WiFi.RSSI(bestConnection)) + String(("(")) + String(constrain((100.0 + WiFi.RSSI(bestConnection)) * 2, 0, 100)) + String(" %) and BSSID: ") + WiFi.BSSIDstr(bestConnection) + String(" and channel: ") + String(WiFi.channel(bestConnection)));
      }
    }

    if (!_ipConfiguration->dhcpEnabled())
    {
      WiFi.config(_ipConfiguration->ipAddress(), _ipConfiguration->dnsServer(), _ipConfiguration->defaultGateway(), _ipConfiguration->subnet());
    }

    WiFi.begin(_WiFissid, _WiFipass);

    Log->print("WiFi connecting");
    int loop = 0;
    while (!isConnected() && loop < 150)
    {
      Log->print(".");
      delay(100);
      loop++;
    }
    Log->println("");

    if (!isConnected())
    {
      Log->println(F("[ERROR] Failed to connect within 15 seconds"));

      if (_preferences->getBool(preference_restart_on_disconnect, false) && (espMillis() > 60000))
      {
        Log->println(F("[INFO] Restart on disconnect watchdog triggered, rebooting"));
        delay(100);
        restartEsp(RestartReason::RestartOnDisconnectWatchdog);
      }
      else
      {
        Log->println(F("[INFO] Retrying WiFi connection"));
        scan(false, true);
      }

      return false;
    }

    return true;
  }

  return false;
}

void NukiNetwork::onRestDataReceivedCallback(const char *path, WebServer &server)
{

  if (_inst)
  {
    if ((_inst->_networkServicesState == -1) || (_inst->_networkServicesState == -3))
    {
      return;
    }

    if (!server.hasArg("token") || server.arg("token") != _inst->_apitoken->get())
    {
      server.send(401, F("text/html"), "");
      return;
    }

    _inst->onRestDataReceived(path, server);

    for (auto receiver : _inst->_restDataReceivers)
    {
      receiver->onRestDataReceived(path, server);
    }
  }
}

void NukiNetwork::onRestDataReceived(const char *path, WebServer &server)
{
  JsonDocument json;

  if (comparePrefixedPath(path, api_path_bridge_enable_api))
  {
    if (server.hasArg("enable") && (server.arg("enable").toInt() == 0 || server.arg("enable").toInt() == 1))
    {

      if (server.arg("enable").toInt() == 0)
      {
        Log->toFile("API", "Disable REST API");
        _apiEnabled = false;
      }
      else
      {
        Log->toFile("API", "Enable REST API");
        _apiEnabled = true;
      }
      _preferences->putBool(preference_api_enabled, _apiEnabled);
      sendResponse(json);
    }
    else
    {
      sendResponse(json, false, 400);
    }
  }
  else if (comparePrefixedPath(path, api_path_bridge_reboot))
  {
    Log->toFile("API", "Reboot requested via REST API");

    sendResponse(json);
    delay(200);
    restartEsp(RestartReason::RequestedViaApi);
  }
  else if (comparePrefixedPath(path, api_path_bridge_enable_web_server))
  {
    if (server.hasArg("enable") && (server.arg("enable").toInt() == 0 || server.arg("enable").toInt() == 1))
    {

      if (server.arg("enable").toInt() == 0)
      {
        if (!_preferences->getBool(preference_webcfgserver_enabled, true) && !forceEnableWebServer)
        {
          return;
        }
        Log->toFile("API", "Disable Config Web Server, restarting");
        _preferences->putBool(preference_webcfgserver_enabled, false);
      }
      else
      {
        if (_preferences->getBool(preference_webcfgserver_enabled, true) || forceEnableWebServer)
        {
          return;
        }
        Log->toFile("API", "Enable Config Web Server, restarting");
        _preferences->putBool(preference_webcfgserver_enabled, true);
      }
      sendResponse(json);

      clearWifiFallback();
      delay(200);
      restartEsp(RestartReason::ReconfigureWebServer);
    }
    else
    {
      sendResponse(json, false, 400);
    }
  }
}

void NukiNetwork::registerRestDataReceiver(RestDataReceiver *receiver)
{
  _restDataReceivers.push_back(receiver);
}

void NukiNetwork::disableAutoRestarts()
{
  _networkTimeout = 0;
  _restartOnDisconnect = false;
}

int NukiNetwork::NetworkServicesState()
{
  return _networkServicesState;
}

bool NukiNetwork::isHAEnabled()
{
  return _homeAutomationEnabled;
}

int8_t NukiNetwork::signalStrength()
{
  return (_networkDeviceType == NetworkDeviceType::ETH ? -1 : WiFi.RSSI());
}

const String NukiNetwork::localIP() const
{
  return (_networkDeviceType == NetworkDeviceType::ETH ? ETH.localIP().toString() : WiFi.localIP().toString());
}

void NukiNetwork::sendToHAFloat(const char *path, const char *query, const float value, uint8_t precision)
{
  char buffer[30];
  dtostrf(value, 0, precision, buffer);
  sendRequestToHA(path, query, buffer);
}

void NukiNetwork::sendToHAInt(const char *path, const char *query, const int value)
{
  char buffer[30];
  itoa(value, buffer, 10);
  sendRequestToHA(path, query, buffer);
}

void NukiNetwork::sendToHAUInt(const char *path, const char *query, const unsigned int value)
{
  char buffer[30];
  utoa(value, buffer, 10);
  sendRequestToHA(path, query, buffer);
}

void NukiNetwork::sendToHAULong(const char *path, const char *query, const unsigned long value)
{
  char buffer[30];
  ultoa(value, buffer, 10);
  sendRequestToHA(path, query, buffer);
}

void NukiNetwork::sendToHALongLong(const char *path, const char *query, const int64_t value)
{
  char buffer[30];
  lltoa(value, buffer, 10);
  sendRequestToHA(path, query, buffer);
}

void NukiNetwork::sendToHABool(const char *path, const char *query, const bool value)
{
  char buffer[2] = {0};
  buffer[0] = value ? '1' : '0';
  sendRequestToHA(path, query, buffer);
}

void NukiNetwork::sendToHAString(const char *path, const char *query, const char *value)
{
  sendRequestToHA(path, query, value);
}

void NukiNetwork::sendRequestToHA(const char *path, const char *query, const char *value)
{
  const size_t BUFFER_SIZE = 385;
  char url[BUFFER_SIZE];

  // Build base URL
  snprintf(url, BUFFER_SIZE, "http://");

  // If user name and password are available, add authentication
  if (_homeAutomationUser && _homeAutomationPassword)
  {
    strncat(url, _homeAutomationUser.c_str(), BUFFER_SIZE - strlen(url) - 1);
    strncat(url, ":", BUFFER_SIZE - strlen(url) - 1);
    strncat(url, _homeAutomationPassword.c_str(), BUFFER_SIZE - strlen(url) - 1);
    strncat(url, "@", BUFFER_SIZE - strlen(url) - 1);
  }

  // Add host + port
  strncat(url, _homeAutomationAdress.c_str(), BUFFER_SIZE - strlen(url) - 1);
  if (_homeAutomationPort)
  {
    char portStr[6]; // Max. 5 digits + zero termination
    snprintf(portStr, sizeof(portStr), ":%d", _homeAutomationPort);
    strncat(url, portStr, BUFFER_SIZE - strlen(url) - 1);
  }

  // Add Path, Query & Value (if available)
  if (path && *path)
  {
    strncat(url, "/", BUFFER_SIZE - strlen(url) - 1);
    strncat(url, path, BUFFER_SIZE - strlen(url) - 1);
  }
  if (query && *query)
  {
    strncat(url, "/", BUFFER_SIZE - strlen(url) - 1);
    strncat(url, query, BUFFER_SIZE - strlen(url) - 1);
  }
  if (value && *value)
  {
    strncat(url, value, BUFFER_SIZE - strlen(url) - 1);
  }

  // Send HTTP request
  _httpClient->begin(url);
  int httpCode = _httpClient->GET();

  if (httpCode > 0)
  {
    Log->println(_httpClient->getString());
  }
  else
  {
    Log->printf(F("[ERROR] HTTP request failed: %s\n"), _httpClient->errorToString(httpCode).c_str());
  }

  _httpClient->end();
}

void NukiNetwork::sendResponse(JsonDocument &jsonResult, bool success, int httpCode)
{
  jsonResult[F("success")] = success ? 1 : 0;
  jsonResult[F("error")] = success ? 0 : httpCode;

  serializeJson(jsonResult, _buffer, _bufferSize);
  _server->send(httpCode, F("application/json"), _buffer);
}

void NukiNetwork::sendResponse(const char *jsonResultStr)
{
  _server->send(200, F("application/json"), jsonResultStr);
}

bool NukiNetwork::comparePrefixedPath(const char *fullPath, const char *subPath)
{
  char prefixedPath[385];
  buildApiPath(subPath, prefixedPath);
  return strcmp(fullPath, prefixedPath) == 0;
}

void NukiNetwork::buildApiPath(const char *path, char *outPath)
{
  // Copy (_apiBridgePath) to outPath
  strncpy(outPath,_apiBridgePath, sizeof(_apiBridgePath) - 1);

  // Append the (path) zo outPath
  strncat(outPath, path, 384 - strlen(outPath)); // Sicherheitsgrenze beachten
}
