/*
  SDWebServer - Example WebServer with SD Card backend for esp8266

  Copyright (c) 2015 Hristo Gochkov. All rights reserved.
  This file is part of the ESP8266WebServer library for Arduino environment.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

  Have a FAT Formatted SD Card connected to the SPI port of the ESP8266
  The web root is the SD Card root folder
  File extensions with more than 3 charecters are not supported by the SD Library
  File Names longer than 8 charecters will be truncated by the SD library, so keep filenames shorter
  index.htm is the default index (works on subfolders as well)

  upload the contents of SdRoot to the root of the SDcard and access the editor by going to http://esp8266sd.local/edit

*/
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <SPI.h>
#include <SD.h>

#include <OneWire.h>
#include <DallasTemperature.h>

#define DBG_OUTPUT_PORT Serial
#define relay_quantity 4  // Количество подключенных реле

byte relay[] = {5, 0, 15, 3};

const char *ssid = "wifiESP";  // имя точки доступа, которую будет раздавать ESP

OneWire oneWire(2);
DallasTemperature ds(&oneWire);
byte num;

ESP8266WebServer server(80);  // порт работы


static bool hasSD = false;
File uploadFile;


void returnOK() {
  server.send(200, "text/plain", "");
}

void returnFail(String msg) {
  server.send(500, "text/plain", msg + "\r\n");
}

bool loadFromSdCard(String path){
  String dataType = "text/plain";
  if(path.endsWith("/")) path += "index.html";

  if(path.endsWith(".src")) path = path.substring(0, path.lastIndexOf("."));
  else if(path.endsWith(".html")) dataType = "text/html";
  else if(path.endsWith(".css")) dataType = "text/css";
  else if(path.endsWith(".js")) dataType = "application/javascript";
  else if(path.endsWith(".png")) dataType = "image/png";
  else if(path.endsWith(".gif")) dataType = "image/gif";
  else if(path.endsWith(".jpg")) dataType = "image/jpeg";
  else if(path.endsWith(".ico")) dataType = "image/x-icon";
  else if(path.endsWith(".xml")) dataType = "text/xml";
  else if(path.endsWith(".pdf")) dataType = "application/pdf";
  else if(path.endsWith(".zip")) dataType = "application/zip";

  File dataFile = SD.open(path.c_str());
  if(dataFile.isDirectory()){
    path += "/index.html";
    dataType = "text/html";
    dataFile = SD.open(path.c_str());
  }

  if (!dataFile)
    return false;

  if (server.hasArg("download")) dataType = "application/octet-stream";

  if (server.streamFile(dataFile, dataType) != dataFile.size()) {
    DBG_OUTPUT_PORT.println("Sent less data than expected!");
  }

  dataFile.close();
  return true;
}

void handleFileUpload(){
  if(server.uri() != "/edit") return;
  HTTPUpload& upload = server.upload();
  if(upload.status == UPLOAD_FILE_START){
    if(SD.exists((char *)upload.filename.c_str())) SD.remove((char *)upload.filename.c_str());
    uploadFile = SD.open(upload.filename.c_str(), FILE_WRITE);
    DBG_OUTPUT_PORT.print("Upload: START, filename: "); DBG_OUTPUT_PORT.println(upload.filename);
  } else if(upload.status == UPLOAD_FILE_WRITE){
    if(uploadFile) uploadFile.write(upload.buf, upload.currentSize);
    DBG_OUTPUT_PORT.print("Upload: WRITE, Bytes: "); DBG_OUTPUT_PORT.println(upload.currentSize);
  } else if(upload.status == UPLOAD_FILE_END){
    if(uploadFile) uploadFile.close();
    DBG_OUTPUT_PORT.print("Upload: END, Size: "); DBG_OUTPUT_PORT.println(upload.totalSize);
  }
}

void deleteRecursive(String path){
  File file = SD.open((char *)path.c_str());
  if(!file.isDirectory()){
    file.close();
    SD.remove((char *)path.c_str());
    return;
  }

  file.rewindDirectory();
  while(true) {
    File entry = file.openNextFile();
    if (!entry) break;
    String entryPath = path + "/" +entry.name();
    if(entry.isDirectory()){
      entry.close();
      deleteRecursive(entryPath);
    } else {
      entry.close();
      SD.remove((char *)entryPath.c_str());
    }
    yield();
  }

  SD.rmdir((char *)path.c_str());
  file.close();
}

void handleDelete(){
  if(server.args() == 0) return returnFail("BAD ARGS");
  String path = server.arg(0);
  if(path == "/" || !SD.exists((char *)path.c_str())) {
    returnFail("BAD PATH");
    return;
  }
  deleteRecursive(path);
  returnOK();
}

void handleCreate(){
  if(server.args() == 0) return returnFail("BAD ARGS");
  String path = server.arg(0);
  if(path == "/" || SD.exists((char *)path.c_str())) {
    returnFail("BAD PATH");
    return;
  }

  if(path.indexOf('.') > 0){
    File file = SD.open((char *)path.c_str(), FILE_WRITE);
    if(file){
      file.write((const char *)0);
      file.close();
    }
  } else {
    SD.mkdir((char *)path.c_str());
  }
  returnOK();
}

void printDirectory() {
  if(!server.hasArg("dir")) return returnFail("BAD ARGS");
  String path = server.arg("dir");
  if(path != "/" && !SD.exists((char *)path.c_str())) return returnFail("BAD PATH");
  File dir = SD.open((char *)path.c_str());
  path = String();
  if(!dir.isDirectory()){
    dir.close();
    return returnFail("NOT DIR");
  }
  dir.rewindDirectory();
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/json", "");
  WiFiClient client = server.client();

  server.sendContent("[");
  for (int cnt = 0; true; ++cnt) {
    File entry = dir.openNextFile();
    if (!entry)
    break;

    String output;
    if (cnt > 0)
      output = ',';

    output += "{\"type\":\"";
    output += (entry.isDirectory()) ? "dir" : "file";
    output += "\",\"name\":\"";
    output += entry.name();
    output += "\"";
    output += "}";
    server.sendContent(output);
    entry.close();
 }
 server.sendContent("]");
 dir.close();
}

void handleNotFound(){
  if(hasSD && loadFromSdCard(server.uri())) return;
  String message = "SDCARD Not Detected\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET)?"GET":"POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i=0; i<server.args(); i++){
    message += " NAME:"+server.argName(i) + "\n VALUE:" + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
  DBG_OUTPUT_PORT.print(message);
}

void relay_switch() {
  int relay_num = (server.arg("val")).toInt(); // Получаем значение ssdp из запроса сохраняем в глобальной переменной
  byte state;
  if (digitalRead(relay[relay_num]))
    state = 0;
  else
    state = 1;
  digitalWrite(relay[relay_num], state);
  server.send(200, "text/plain", String(state)); // отправляем ответ о выполнении
}

void relay_status() {
  int relay_num = (server.arg("val")).toInt(); // Получаем значение ssdp из запроса сохраняем в глобальной переменной
  byte state;
  if (digitalRead(relay[relay_num]))
    state = 1;
  else
    state = 0;
  server.send(200, "text/plain", String(state)); // отправляем ответ о выполнении
}

String check_temp() {
  ds.requestTemperatures();
  return String(ds.getTempCByIndex(0));
}

void setup(void){
  for (byte i = 0; i < relay_quantity; i++) {
    pinMode(relay[i], OUTPUT);
  }
  

  DBG_OUTPUT_PORT.begin(115200);
  DBG_OUTPUT_PORT.setDebugOutput(true);
  DBG_OUTPUT_PORT.print("\n");

  ds.begin();
  num = ds.getDeviceCount();

  WiFi.softAP(ssid);  // создаем точку доступа

  server.begin();                    // инициализируем Web-server

  DBG_OUTPUT_PORT.print("\nMy IP to connect via Web-Browser or FTP: ");
  DBG_OUTPUT_PORT.println(WiFi.softAPIP());  // выводим локальный IP-адресс
  DBG_OUTPUT_PORT.println("\n");

  // Wait for connection
  // uint8_t i = 0;
  // while (WiFi.status() != WL_CONNECTED && i++ < 20) {//wait 10 seconds
  //   delay(500);
  // }
  // if(i == 21){
  //   DBG_OUTPUT_PORT.print("Could not connect to");
  //   DBG_OUTPUT_PORT.println(ssid);
  //   while(1) delay(500);
  // }
  DBG_OUTPUT_PORT.print("Connected! IP address: ");
  DBG_OUTPUT_PORT.println(WiFi.localIP());

  server.on("/list", HTTP_GET, printDirectory);
  server.on("/edit", HTTP_DELETE, handleDelete);
  server.on("/edit", HTTP_PUT, handleCreate);
  server.on("/edit", HTTP_POST, [](){ returnOK(); }, handleFileUpload);
  server.on("/relay_switch", relay_switch);     // переключать состояние реле по запросу вида /relay_switch?val=relay_id
  server.on("/relay_status", relay_status);     // переключать состояние реле по запросу вида /relay_switch?val=relay_id
  // server.on("/relay_switch", []() {                     // при HTTP-запросе вида http://192.168.4.1/relay_switch
  //   server.send(200, "text/plain", relay_switch());     // отдаем клиенту код успешной обработки запроса(200) (обязательно возвращать код ответа), сообщаем, что формат возвращаемого ответа является текстовым. А так же вызываем фунцию переключения реле
  // });
  // server.on("/relay_status", []() {
  //   server.send(200, "text/plain", relay_status());
  // });
  server.on("/check_temp", []() {
    server.send(200, "text/plain", check_temp());
  });
  server.onNotFound(handleNotFound);

  server.begin();
  DBG_OUTPUT_PORT.println("HTTP server started");

  if (SD.begin(4)){
     DBG_OUTPUT_PORT.println("SD Card initialized.");
     hasSD = true;
  }
}

void loop(void){
  server.handleClient();
}