#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <math.h>

// ================= WiFi 设置 =================
const char* ssid = "yours";
const char* password = "yours";

// ================= Telegram 设置 =================
const char* botToken = "yours";
const char* chatID = "yours";

// ================= 引脚定义 =================
#define TDS_PIN 0
#define TEMP_PIN 1
#define FLOW_PIN 4
#define LED_PIN 2

// ================= 全局变量 =================
WebServer server(80);
Preferences prefs;

volatile unsigned long flowPulseCount = 0;
float flowRate = 0;
float totalLiters = 0;
unsigned long lastFlowCalc = 0;

float tdsOffset = 0; // 校准偏移
const int historySize = 30;
float tdsHistory[historySize] = {0};
float tempHistory[historySize] = {0};
float flowHistory[historySize] = {0};
int historyIndex = 0;

// ================= Telegram 60分钟报警 =================
const int tdsAlertSize = 1800; // 60分钟 * 每2秒采样
float tdsAlertBuffer[tdsAlertSize] = {0};
int tdsAlertIndex = 0;
bool alertSent = false;

// ================= 日累计流量 =================
const int maxDays = 30;
float dailyFlow[maxDays] = {0};
int todayIndex = 0;
unsigned long lastMidnight = 0;

// ================= NTP 时间 =================
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP,"pool.ntp.org",8*3600,60000);

// ================= 函数声明 =================
void IRAM_ATTR flowISR() { flowPulseCount++; }
void handleRoot();
void handleData();
void handleCalibrate();
void handleDailyFlow();
void handleTime();
void updateHistory(float tdsVal, float tempVal, float flowVal);
void updateDailyFlow();
void sendTelegram(String message);

// ================= 初始化 =================
void setup() {
  Serial.begin(115200);
  pinMode(FLOW_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN), flowISR, RISING);

  prefs.begin("flowData", false);
  for(int i=0;i<maxDays;i++){
    char key[10];
    sprintf(key,"day%d",i);
    dailyFlow[i] = prefs.getFloat(key,0.0);
  }
  todayIndex = prefs.getInt("todayIndex",0);

  WiFi.begin(ssid,password);
  Serial.print("连接 WiFi");
  while(WiFi.status()!=WL_CONNECTED){delay(500);Serial.print(".");}
  Serial.println("\nWiFi 已连接，IP: "+WiFi.localIP().toString());

  timeClient.begin();

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/calibrate", handleCalibrate);
  server.on("/dailyFlow", handleDailyFlow);
  server.on("/time", handleTime);
  server.begin();
  Serial.println("Web 服务器已启动");
}

// ================= 主循环 =================
void loop() {
  server.handleClient();
  timeClient.update();

  if(millis()-lastFlowCalc>=1000){
    detachInterrupt(FLOW_PIN);
    unsigned long pulses = flowPulseCount;
    flowPulseCount=0;
    attachInterrupt(digitalPinToInterrupt(FLOW_PIN), flowISR, RISING);

    flowRate = (float)pulses/450.0; // L/s
    totalLiters += flowRate;
    flowRate *= 60.0; // L/min
    lastFlowCalc=millis();
  }

  updateDailyFlow();
}

// ================= 历史数据更新 =================
void updateHistory(float tdsVal, float tempVal, float flowVal){
  tdsHistory[historyIndex] = tdsVal;
  tempHistory[historyIndex] = tempVal;
  flowHistory[historyIndex] = flowVal;
  historyIndex=(historyIndex+1)%historySize;

  tdsAlertBuffer[tdsAlertIndex]=tdsVal;
  tdsAlertIndex=(tdsAlertIndex+1)%tdsAlertSize;

  if(!alertSent){
    bool overLimit=true;
    for(int i=0;i<tdsAlertSize;i++){ if(tdsAlertBuffer[i]<=10){overLimit=false;break;} }
    if(overLimit){ sendTelegram("⚠️ TDS已连续60分钟超过10 ppm！"); alertSent=true; }
  }else{
    bool anyBelow=false;
    for(int i=0;i<tdsAlertSize;i++){ if(tdsAlertBuffer[i]<=10){anyBelow=true;break;} }
    if(anyBelow) alertSent=false;
  }
}

// ================= 日累计流量更新 =================
void updateDailyFlow(){
  if(millis()-lastMidnight>=24UL*60*60*1000){
    char key[10];
    sprintf(key,"day%d",todayIndex);
    prefs.putFloat(key, dailyFlow[todayIndex]);

    todayIndex=(todayIndex+1)%maxDays;
    prefs.putInt("todayIndex",todayIndex);
    totalLiters=0;
    lastMidnight=millis();
  }
}

// ================= Telegram发送 =================
void sendTelegram(String message){
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  String url = "https://api.telegram.org/bot" + String(botToken) + "/sendMessage?chat_id=" + String(chatID) + "&text=" + message;
  if(https.begin(client, url.c_str())){
    https.GET();
    https.end();
  }
}

// ================= 网页 =================
void handleRoot(){
  String html = R"rawliteral(
<!DOCTYPE html><html lang="zh">
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-C3 智能水质监控</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
<style>
body{font-family:Segoe UI,Arial;background:#f7f9fb;margin:0;padding:10px;}
.card{background:#fff;border-radius:16px;box-shadow:0 4px 8px rgba(0,0,0,0.1);padding:16px;margin:10px 0;}
h2{text-align:center;color:#0078d7;}
button{background:#0078d7;color:white;border:none;padding:10px 18px;border-radius:8px;font-size:16px;}
#tdsValue{font-size:32px;color:#0078d7;}
#tempValue,#flowValue,#wifiValue,#timeValue{font-size:20px;}
input{width:60px;text-align:center;font-size:16px;}
canvas{max-width:100%;margin:0 auto;}
#tdsBarContainer{background:#e0e0e0;border-radius:12px;height:24px;position:relative;margin-bottom:8px;}
#tdsBar{background:#0078d7;height:100%;width:0%;border-radius:12px;}
</style>
<body>
<h2>ESP32-C3 智能水质监控</h2>

<div class="card">
  <div id="tdsBarContainer"><div id="tdsBar"></div></div>
  <div id="tdsValue">-- ppm</div>
  校准用水 TDS：<input type="number" id="tdsInput" value="4">
  <button onclick="calibrate()">校准</button>
</div>

<div class="card">
  🌡 温度：<span id="tempValue">-- ℃</span><br>
  💦 流量：<span id="flowValue">-- L/min</span><br>
  📶 WiFi 信号：<span id="wifiValue">-- dBm</span>
</div>

<div class="card">
  <h3>当前时间</h3>
  <div id="timeValue">--</div>
</div>

<div class="card">
  <canvas id="historyChart" height="150"></canvas>
</div>

<div class="card">
  <h3>每日总流量 (L)</h3>
  <canvas id="dailyFlowChart" height="150"></canvas>
</div>

<div class="card" id="sysinfo"></div>

<script>
let historyCtx=document.getElementById('historyChart').getContext('2d');
let historyChart=new Chart(historyCtx,{type:'line',data:{labels:Array(30).fill(''),
datasets:[{label:'TDS ppm',data:Array(30).fill(0),borderColor:'#0078d7',fill:false},
{label:'温度 ℃',data:Array(30).fill(0),borderColor:'#ff5733',fill:false},
{label:'流量 L/min',data:Array(30).fill(0),borderColor:'#33c4ff',fill:false}]},
options:{responsive:true,animation:false,scales:{y:{beginAtZero:true}}}});

let dailyCtx=document.getElementById('dailyFlowChart').getContext('2d');
let dailyChart=new Chart(dailyCtx,{type:'bar',data:{labels:Array(30).fill(''),
datasets:[{label:'总流量(L)',data:Array(30).fill(0),backgroundColor:'#33c4ff'}]},
options:{responsive:true,animation:false,scales:{y:{beginAtZero:true}}}});

function updateData(){
  fetch('/data').then(r=>r.json()).then(d=>{
    document.getElementById('tdsValue').innerText=d.tds+' ppm';
    document.getElementById('tdsBar').style.width=Math.min(d.tds/1000*100,100)+'%';
    document.getElementById('tempValue').innerText=d.temp+' ℃';
    document.getElementById('flowValue').innerText=d.flow+' L/min (累计 '+d.total+' L)';
    document.getElementById('wifiValue').innerText=d.wifi+' dBm';
    document.getElementById('sysinfo').innerHTML=`🕒 运行时间：${d.uptime}s<br>💾 内存：${d.memory} KB<br>🌐 IP：${d.ip}`;

    historyChart.data.datasets[0].data.push(d.tds); historyChart.data.datasets[0].data.shift();
    historyChart.data.datasets[1].data.push(d.temp); historyChart.data.datasets[1].data.shift();
    historyChart.data.datasets[2].data.push(d.flow); historyChart.data.datasets[2].data.shift();
    historyChart.update();
  });
}

function fetchDailyFlow(){
  fetch('/dailyFlow').then(r=>r.json()).then(d=>{
    dailyChart.data.datasets[0].data = d.flow;
    dailyChart.data.labels = d.dates;
    dailyChart.update();
  });
}

function updateTime(){
  fetch('/time').then(r=>r.json()).then(d=>{
    document.getElementById('timeValue').innerText=d.time;
  });
}

function calibrate(){
  let val=document.getElementById('tdsInput').value;
  fetch('/calibrate?value='+val).then(()=>alert('清水校准完成！'));
}

setInterval(updateData,2000);
setInterval(fetchDailyFlow,60000);
setInterval(updateTime,1000);
updateData(); fetchDailyFlow(); updateTime();
</script>
</body>
</html>
)rawliteral";

  server.send(200,"text/html",html);
}

// ================= 数据接口 =================
void handleData(){
  float tdsRaw=analogRead(TDS_PIN);
  float tempRaw=analogRead(TEMP_PIN);

  float tdsVoltage=tdsRaw/4095.0*3.3;
  float tempVoltage=tempRaw/4095.0*3.3;

  float tdsValue=tdsVoltage*500.0 - tdsOffset;
  if(tdsValue<0) tdsValue=0;
  if(tdsValue>999) tdsValue=999;

  float Rref=10000.0;
  float Rntc = Rref*(3.3/tempVoltage-1);
  float T0=298.15;
  float B=3950;
  float temperature=1.0/(1.0/T0 + (1.0/B)*log(Rntc/10000.0))-273.15;

  updateHistory(tdsValue,temperature,flowRate);

  String json="{";
  json+="\"tds\":"+String(tdsValue,1)+",";
  json+="\"temp\":"+String(temperature,1)+",";
  json+="\"flow\":"+String(flowRate,2)+",";
  json+="\"total\":"+String(totalLiters,2)+",";
  json+="\"wifi\":"+String(WiFi.RSSI())+",";
  json+="\"ip\":\""+WiFi.localIP().toString()+"\",";
  json+="\"memory\":"+String(ESP.getFreeHeap()/1024)+",";
  json+="\"uptime\":"+String(millis()/1000);
  json+="}";
  server.send(200,"application/json",json);
}

// ================= 校准 =================
void handleCalibrate(){
  float userTDS=server.arg("value").toFloat();
  float tdsRaw=analogRead(TDS_PIN)/4095.0*3.3;
  tdsOffset=tdsRaw*500.0-userTDS;
  server.send(200,"text/plain","ok");
}

// ================= 日累计流量接口 =================
void handleDailyFlow(){
  String json="{\"flow\":[";
  for(int i=0;i<maxDays;i++){
    json+=String(dailyFlow[i],2);
    if(i<maxDays-1) json+=",";
  }
  json+="],\"dates\":[";
  time_t now = timeClient.getEpochTime();
  for(int i=0;i<maxDays;i++){
    time_t dayTime = now - (maxDays-1-i)*24*3600;
    struct tm *tmstruct = localtime(&dayTime);
    char buf[6];
    sprintf(buf,"%02d-%02d",tmstruct->tm_mon+1,tmstruct->tm_mday);
    json += String(buf);
    if(i<maxDays-1) json+=",";
  }
  json+="]}";
  server.send(200,"application/json",json);
}

// ================= 时间接口 =================
void handleTime(){
  timeClient.update();
  time_t rawtime = timeClient.getEpochTime();
  struct tm *timeinfo = localtime(&rawtime);

  char buf[20];
  sprintf(buf, "%04d-%02d-%02d %02d:%02d:%02d",
          1900 + timeinfo->tm_year,
          timeinfo->tm_mon + 1,
          timeinfo->tm_mday,
          timeinfo->tm_hour,
          timeinfo->tm_min,
          timeinfo->tm_sec);

  String json = "{\"time\":\"" + String(buf) + "\"}";
  server.send(200,"application/json",json);
}
