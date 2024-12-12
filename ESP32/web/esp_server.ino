#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>

// WiFi credentials
const char* ssid = "lin";
const char* password = "01630184";

// Admin credentials
const char* admin_user = "admin";
const char* admin_pass = "admin";

// Web server on port 80
WebServer server(80);

// Configuration storage
struct Config {
  String config1;
  String config2;
  String config3;
  String config4;
  String config5;
} currentConfig;

void setup() {
  Serial.begin(115200);
  
  // Initialize SPIFFS for serving static files
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS initialization failed!");
    return;
  }

  // Connect to WiFi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Connecting to WiFi...");
  }
  Serial.println("Connected to WiFi");
  Serial.println(WiFi.localIP());

  // Define server routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/index.html", HTTP_GET, handleRoot);
  server.on("/login.html", HTTP_GET, handleLogin);
  server.on("/login", HTTP_POST, handleLoginPost);
  server.on("/check-auth", HTTP_GET, handleCheckAuth);
  server.on("/save-config", HTTP_POST, handleSaveConfig);
  
  // Start server
  server.begin();
}

void loop() {
  server.handleClient();
}

// Route handlers
void handleRoot() {
  if (SPIFFS.exists("/index.html")) {
    File file = SPIFFS.open("/index.html", "r");
    server.streamFile(file, "text/html");
    file.close();
  } else {
    server.send(404, "text/plain", "File not found");
  }
}

void handleLogin() {
  if (SPIFFS.exists("/login.html")) {
    File file = SPIFFS.open("/login.html", "r");
    server.streamFile(file, "text/html");
    file.close();
  } else {
    server.send(404, "text/plain", "File not found");
  }
}

void handleLoginPost() {
  if (server.hasArg("plain")) {
    StaticJsonDocument<200> doc;
    DeserializationError error = deserializeJson(doc, server.arg("plain"));
    
    if (error) {
      server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid JSON\"}");
      return;
    }

    const char* username = doc["username"];
    const char* password = doc["password"];

    if (!username || !password) {
      server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Missing credentials\"}");
      return;
    }

    if (strcmp(username, admin_user) == 0 && strcmp(password, admin_pass) == 0) {
      server.send(200, "application/json", "{\"status\":\"success\"}");
    } else {
      server.send(401, "application/json", "{\"status\":\"error\",\"message\":\"Invalid username or password\"}");
    }
  }
}

void handleCheckAuth() {
  // In a real implementation, you would check session/token
  server.send(200, "application/json", "{\"status\":\"success\"}");
}

void handleSaveConfig() {
  if (server.hasArg("plain")) {
    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, server.arg("plain"));
    
    if (error) {
      server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid JSON\"}");
      return;
    }

    // Update configuration
    currentConfig.config1 = doc["config1"] | "";
    currentConfig.config2 = doc["config2"] | "";
    currentConfig.config3 = doc["config3"] | "";
    currentConfig.config4 = doc["config4"] | "";
    currentConfig.config5 = doc["config5"] | "";

    // Print saved configuration
    Serial.println("Saved configuration:");
    Serial.println("Config1: " + currentConfig.config1);
    Serial.println("Config2: " + currentConfig.config2);
    Serial.println("Config3: " + currentConfig.config3);
    Serial.println("Config4: " + currentConfig.config4);
    Serial.println("Config5: " + currentConfig.config5);

    server.send(200, "application/json", "{\"status\":\"success\"}");
  }
}