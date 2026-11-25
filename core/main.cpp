// Native Windows camera capture and minimal localhost WebSocket emitter.
// OpenCV is used for frame acquisition and laser detection.
// WebSocket is intentionally implemented with WinSock to avoid a second runtime.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <wincrypt.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <opencv2/opencv.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <conio.h>
#include <cstring>
#include <array>
#include <iomanip>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "shell32.lib")

namespace {
std::mutex socketMutex;
SOCKET clientSocket = INVALID_SOCKET;
std::atomic<bool> running{true};
HWND previewWindow = nullptr;
HWND toolbarButtons[7]={};
HWND presetButtons[5]={};
HWND calibrateButton=nullptr,clearCalibrationButton=nullptr;
HWND zeroButton=nullptr,clearZeroButton=nullptr;
HWND offsetXControl=nullptr,offsetYControl=nullptr,offsetXLabel=nullptr,offsetYLabel=nullptr;
HWND settingsStatus=nullptr;
HWND zoomLabels[3]={},zoomTracks[3]={};
HWND hardwarePropertyLabel=nullptr,hardwarePropertyCombo=nullptr;
HWND hardwareValueLabel=nullptr,hardwareValueTrack=nullptr,hardwareApplyButton=nullptr;
HWND opticsStatus=nullptr;
int digitalZoomTimes100=100,zoomCenterXTimes1000=500,zoomCenterYTimes1000=500;
bool opticsChanged=false;
struct HardwareProperty {
    const char* name;
    int cvProperty;
    int minimum;
    int maximum;
    const char* description;
};
const HardwareProperty hardwareProperties[]={
    {"Zoom (driver)",cv::CAP_PROP_ZOOM,0,500,"May be optical OR digital depending on hardware"},
    {"Pan",cv::CAP_PROP_PAN,-180,180,"Driver controlled horizontal movement"},
    {"Tilt",cv::CAP_PROP_TILT,-180,180,"Driver controlled vertical movement"},
    {"Gain",cv::CAP_PROP_GAIN,0,255,"Sensor amplification, may add noise"},
    {"Brightness",cv::CAP_PROP_BRIGHTNESS,0,255,"Camera brightness, not detector threshold"},
    {"Contrast",cv::CAP_PROP_CONTRAST,0,255,"Driver image contrast"},
    {"Saturation",cv::CAP_PROP_SATURATION,0,255,"Color saturation"},
    {"Sharpness",cv::CAP_PROP_SHARPNESS,0,255,"Image sharpening may increase noise"},
    {"White balance temp",cv::CAP_PROP_WB_TEMPERATURE,2000,9000,"Color temperature; driver must support it"},
    {"Gamma",cv::CAP_PROP_GAMMA,0,500,"Tonal response"},
    {"Hue",cv::CAP_PROP_HUE,-180,180,"Color hue"},
    {"Backlight",cv::CAP_PROP_BACKLIGHT,0,10,"Backlight correction"},
    {"Focus",cv::CAP_PROP_FOCUS,0,1023,"Lens focus; disable auto focus first"},
    {"Exposure",cv::CAP_PROP_EXPOSURE,-13,0,"DSHOW exposure log2 values; disable auto exposure first"}
};
constexpr int hardwarePropertyCount=sizeof(hardwareProperties)/sizeof(hardwareProperties[0]);
int requestedHardwareProperty=-1,requestedHardwareValue=0;
bool hardwareApplyPending=false;
std::string opticsFeedback="Select a driver property, then Apply. Unsupported properties may fail.";

HWND resolutionCombo=nullptr,fpsCombo=nullptr,resolutionLabel=nullptr,fpsLabel=nullptr;
enum PanelView { NoPanel, PresetPanel, SettingsPanel, CalibrationPanel, ZeroPanel, OpticsPanel };
enum UiButton { CameraButtonId=2100, PresetButtonId, SettingsButtonId, CalibrationButtonId, ZeroButtonId, GameButtonId, OpticsButtonId,
PresetOneId=2120,PresetTwoId,PresetThreeId,PresetSaveId,PresetLoadId,
CalibrateId=2130,ClearCalibrationId,ZeroStartId,ZeroClearId,
CameraResolutionId=2140,CameraFpsId,CameraModeApplyId,
ZoomTrackId=2150,ZoomCenterXId,ZoomCenterYId,HardwareComboId,HardwareValueId,HardwareApplyId };
PanelView activePanel=NoPanel;
bool maskView=false;
int calibrationStep=-1;
int zeroingStep=0;
cv::Point2f screenCorners[4]={};
int cornerCount=0;
double zeroOffsetX=0,zeroOffsetY=0;
cv::Point2f lastLaserRaw(0,0),lastLaserCorrected(0,0);
bool lastLaserValid=false;
RECT cameraViewport={};
std::string settingsIniPath;
std::atomic<int> uiAction{0};

int iniInt(const char* section,const char* key,int fallback){
    return static_cast<int>(GetPrivateProfileIntA(section,key,fallback,settingsIniPath.c_str()));
}
double iniDouble(const char* section,const char* key,double fallback){
    char buffer[80]={};
    GetPrivateProfileStringA(section,key,"",buffer,sizeof(buffer),settingsIniPath.c_str());
    if(!buffer[0])return fallback;
    try{return std::stod(buffer);}catch(...){return fallback;}
}
void saveInt(const char* section,const char* key,int value){
    WritePrivateProfileStringA(section,key,std::to_string(value).c_str(),settingsIniPath.c_str());
}
void saveDouble(const char* section,const char* key,double value){
    WritePrivateProfileStringA(section,key,std::to_string(value).c_str(),settingsIniPath.c_str());
}
void persistCalibration(){
    saveInt("calibration","count",cornerCount);
    for(int index=0;index<cornerCount;index++){
        saveDouble("calibration",("x"+std::to_string(index)).c_str(),screenCorners[index].x);
        saveDouble("calibration",("y"+std::to_string(index)).c_str(),screenCorners[index].y);
    }
    saveDouble("calibration","offsetX",zeroOffsetX);
    saveDouble("calibration","offsetY",zeroOffsetY);
}
void loadCalibration(){
    cornerCount=std::clamp(iniInt("calibration","count",0),0,4);
    for(int index=0;index<cornerCount;index++){
        screenCorners[index].x=static_cast<float>(iniDouble("calibration",("x"+std::to_string(index)).c_str(),0));
        screenCorners[index].y=static_cast<float>(iniDouble("calibration",("y"+std::to_string(index)).c_str(),0));
    }
    zeroOffsetX=iniDouble("calibration","offsetX",0);
    zeroOffsetY=iniDouble("calibration","offsetY",0);
}
bool correctedPoint(const cv::Point2f& raw,cv::Point2f& corrected){
    corrected=raw;
    if(cornerCount==4){
        const cv::Point2f destination[4]={{0,0},{1,0},{1,1},{0,1}};
        const cv::Mat transform=cv::getPerspectiveTransform(screenCorners,destination);
        const std::vector<cv::Point2f> source={raw};
        std::vector<cv::Point2f> mapped;
        cv::perspectiveTransform(source,mapped,transform);
        if(mapped.empty()||!std::isfinite(mapped[0].x)||!std::isfinite(mapped[0].y))return false;
        corrected=mapped[0];
    }
    corrected.x+=static_cast<float>(zeroOffsetX);
    corrected.y+=static_cast<float>(zeroOffsetY);
    return true;
}
void updateLayout(HWND window);
void updatePanels();
void updateZeroLabels();
void updateOpticsLabels();
void resetOpticsCalibration(){
    cornerCount=0;
    calibrationStep=-1;
    zeroOffsetX=0;zeroOffsetY=0;
    persistCalibration();
}


enum ControlId {
    BrightnessControl=2001, RedDifferenceControl, RedRiseControl, SpotAreaControl,
    FocusControl, ExposureControl, AutoFocusControl, AutoExposureControl
};
struct SliderControl {
    int id;
    const char* caption;
    int minimum;
    int maximum;
    int initial;
    const char* help;
    HWND trackbar=nullptr;
    HWND label=nullptr;
};
SliderControl sliders[]={
    {BrightnessControl,"Brightness",0,255,190,"Red brightness required (0..255)"},
    {RedDifferenceControl,"Red excess",0,160,45,"Red must exceed green and blue"},
    {RedRiseControl,"Red change",1,120,28,"Increase over previous camera frame"},
    {SpotAreaControl,"Spot pixels",1,40,4,"Minimum connected spot area"},
    {FocusControl,"Focus",0,1023,0,"Manual lens position; driver may limit"},
    {ExposureControl,"Exposure", -13,0,-7,"DirectShow exposure in log2 units"}
};
HWND focusAutomaticCheckbox=nullptr;
HWND exposureAutomaticCheckbox=nullptr;
HWND cameraStatusLabel=nullptr;

void layoutPreviewControls(HWND window){
    RECT rect={};GetClientRect(window,&rect);
    const int width=rect.right,height=rect.bottom;
    const int sideLeft=std::max(0,width-368);
    for(int index=0;index<6;index++){
        const bool visible=activePanel==SettingsPanel;
        ShowWindow(sliders[index].label,visible?SW_SHOW:SW_HIDE);
        ShowWindow(sliders[index].trackbar,visible?SW_SHOW:SW_HIDE);
        const int top=62+index*82;
        SetWindowPos(sliders[index].label,nullptr,sideLeft+12,top,350,36,SWP_NOZORDER);
        SetWindowPos(sliders[index].trackbar,nullptr,sideLeft+12,top+34,340,35,SWP_NOZORDER);
    }
    const bool settingsVisible=activePanel==SettingsPanel;
    ShowWindow(focusAutomaticCheckbox,settingsVisible?SW_SHOW:SW_HIDE);
    ShowWindow(exposureAutomaticCheckbox,settingsVisible?SW_SHOW:SW_HIDE);
    ShowWindow(cameraStatusLabel,settingsVisible?SW_SHOW:SW_HIDE);
    SetWindowPos(focusAutomaticCheckbox,nullptr,sideLeft+12,560,160,22,SWP_NOZORDER);
    SetWindowPos(exposureAutomaticCheckbox,nullptr,sideLeft+182,560,170,22,SWP_NOZORDER);
    SetWindowPos(cameraStatusLabel,nullptr,sideLeft+12,594,340,70,SWP_NOZORDER);
    for(int index=0;index<7;index++)
        SetWindowPos(toolbarButtons[index],nullptr,12+index*128,7,122,34,SWP_NOZORDER);
    for(int index=0;index<5;index++){
        ShowWindow(presetButtons[index],activePanel==PresetPanel?SW_SHOW:SW_HIDE);
        SetWindowPos(presetButtons[index],nullptr,sideLeft+12,70+index*58,330,43,SWP_NOZORDER);
    }
    for(HWND button:{calibrateButton,clearCalibrationButton}){
        ShowWindow(button,activePanel==CalibrationPanel?SW_SHOW:SW_HIDE);
    }
    SetWindowPos(calibrateButton,nullptr,sideLeft+12,70,330,43,SWP_NOZORDER);
    SetWindowPos(clearCalibrationButton,nullptr,sideLeft+12,126,330,43,SWP_NOZORDER);
    for(HWND button:{zeroButton,clearZeroButton})
        ShowWindow(button,activePanel==ZeroPanel?SW_SHOW:SW_HIDE);
    SetWindowPos(zeroButton,nullptr,sideLeft+12,70,330,43,SWP_NOZORDER);
    SetWindowPos(clearZeroButton,nullptr,sideLeft+12,126,330,43,SWP_NOZORDER);
    for(HWND control:{offsetXControl,offsetYControl,offsetXLabel,offsetYLabel})
        ShowWindow(control,activePanel==ZeroPanel?SW_SHOW:SW_HIDE);
    SetWindowPos(offsetXLabel,nullptr,sideLeft+12,194,330,22,SWP_NOZORDER);
    SetWindowPos(offsetXControl,nullptr,sideLeft+12,220,330,35,SWP_NOZORDER);
    SetWindowPos(offsetYLabel,nullptr,sideLeft+12,270,330,22,SWP_NOZORDER);
    SetWindowPos(offsetYControl,nullptr,sideLeft+12,296,330,35,SWP_NOZORDER);
    for(HWND control:{resolutionCombo,fpsCombo,resolutionLabel,fpsLabel})
        ShowWindow(control,settingsVisible?SW_SHOW:SW_HIDE);
    SetWindowPos(resolutionLabel,nullptr,sideLeft+12,668,340,20,SWP_NOZORDER);
    SetWindowPos(resolutionCombo,nullptr,sideLeft+12,689,340,180,SWP_NOZORDER);
    SetWindowPos(fpsLabel,nullptr,sideLeft+12,721,340,20,SWP_NOZORDER);
    SetWindowPos(fpsCombo,nullptr,sideLeft+12,741,340,160,SWP_NOZORDER);
    const bool opticsVisible=activePanel==OpticsPanel;
    for(int index=0;index<3;index++){
        ShowWindow(zoomLabels[index],opticsVisible?SW_SHOW:SW_HIDE);
        ShowWindow(zoomTracks[index],opticsVisible?SW_SHOW:SW_HIDE);
        SetWindowPos(zoomLabels[index],nullptr,sideLeft+12,72+index*92,340,44,SWP_NOZORDER);
        SetWindowPos(zoomTracks[index],nullptr,sideLeft+12,112+index*92,340,32,SWP_NOZORDER);
    }
    for(HWND control:{hardwarePropertyLabel,hardwarePropertyCombo,hardwareValueLabel,
        hardwareValueTrack,hardwareApplyButton,opticsStatus})
        ShowWindow(control,opticsVisible?SW_SHOW:SW_HIDE);
    SetWindowPos(hardwarePropertyLabel,nullptr,sideLeft+12,352,340,38,SWP_NOZORDER);
    SetWindowPos(hardwarePropertyCombo,nullptr,sideLeft+12,388,340,205,SWP_NOZORDER);
    SetWindowPos(hardwareValueLabel,nullptr,sideLeft+12,440,340,42,SWP_NOZORDER);
    SetWindowPos(hardwareValueTrack,nullptr,sideLeft+12,480,340,36,SWP_NOZORDER);
    SetWindowPos(hardwareApplyButton,nullptr,sideLeft+12,525,340,38,SWP_NOZORDER);
    SetWindowPos(opticsStatus,nullptr,sideLeft+12,582,340,130,SWP_NOZORDER);
    ShowWindow(settingsStatus,activePanel==NoPanel?SW_HIDE:SW_SHOW);
    SetWindowPos(settingsStatus,nullptr,sideLeft+12,activePanel==SettingsPanel?680:210,340,100,SWP_NOZORDER);
}
void updateZeroLabels(){
    if(!offsetXLabel||!offsetYLabel)return;
    std::ostringstream horizontal,vertical;
    horizontal << "Offset X: " << std::fixed << std::setprecision(3) << zeroOffsetX << " (left / right)";
    vertical << "Offset Y: " << std::fixed << std::setprecision(3) << zeroOffsetY << " (up / down)";
    SetWindowTextA(offsetXLabel,horizontal.str().c_str());
    SetWindowTextA(offsetYLabel,vertical.str().c_str());
}
void updateOpticsLabels(){
    if(!zoomLabels[0]||!hardwareValueLabel)return;
    const int values[]={digitalZoomTimes100,zoomCenterXTimes1000,zoomCenterYTimes1000};
    const char* names[]={"DIGITAL ZOOM: ","Crop center X: ","Crop center Y: "};
    for(int index=0;index<3;index++){
        std::ostringstream label;
        label<<names[index]<<std::fixed<<std::setprecision(index==0?2:3)
             <<static_cast<double>(values[index])/(index==0?100.0:1000.0);
        if(index==0)label<<" x (cropped sensor region, not optical)";
        if(index==1)label<<" | move horizontal ROI";
        if(index==2)label<<" | move vertical ROI";
        SetWindowTextA(zoomLabels[index],label.str().c_str());
    }
    const int propertyIndex=hardwarePropertyCombo?
        static_cast<int>(SendMessageA(hardwarePropertyCombo,CB_GETCURSEL,0,0)):-1;
    if(propertyIndex>=0&&propertyIndex<hardwarePropertyCount){
        const HardwareProperty& property=hardwareProperties[propertyIndex];
        const int value=static_cast<int>(SendMessageA(hardwareValueTrack,TBM_GETPOS,0,0));
        const std::string detail=std::string(property.name)+": requested "+std::to_string(value)
            +" | "+property.description;
        SetWindowTextA(hardwareValueLabel,detail.c_str());
    }
}
void updatePanels(){
    if(previewWindow)layoutPreviewControls(previewWindow);
}
void updateLayout(HWND window){layoutPreviewControls(window);}
void updateSliderLabels() {
    for(auto& setting:sliders){
        if(!setting.label||!setting.trackbar)continue;
        const int value=static_cast<int>(SendMessageA(setting.trackbar,TBM_GETPOS,0,0));
        const std::string text=std::string(setting.caption)+": "+std::to_string(value)+
            " | "+setting.help;
        SetWindowTextA(setting.label,text.c_str());
    }
}
int sliderValue(int id) {
    for(const auto& setting:sliders){
        if(setting.id==id&&setting.trackbar)
            return static_cast<int>(SendMessageA(setting.trackbar,TBM_GETPOS,0,0));
    }
    return 0;
}
bool checkboxChecked(HWND checkbox){
    return SendMessageA(checkbox,BM_GETCHECK,0,0)==BST_CHECKED;
}
void createPreviewControls(HWND window) {
    INITCOMMONCONTROLSEX commonControls={sizeof(commonControls),ICC_BAR_CLASSES};
    InitCommonControlsEx(&commonControls);
    for(auto& setting:sliders){
        setting.label=CreateWindowExA(0,"STATIC","",WS_CHILD|WS_VISIBLE,
            0,0,100,20,window,nullptr,GetModuleHandleA(nullptr),nullptr);
        setting.trackbar=CreateWindowExA(0,TRACKBAR_CLASSA,"",WS_CHILD|WS_VISIBLE|TBS_HORZ|TBS_NOTICKS,
            0,0,100,28,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(setting.id)),
            GetModuleHandleA(nullptr),nullptr);
        SendMessageA(setting.trackbar,TBM_SETRANGEMIN,FALSE,setting.minimum);
        SendMessageA(setting.trackbar,TBM_SETRANGEMAX,TRUE,setting.maximum);
        SendMessageA(setting.trackbar,TBM_SETPOS,TRUE,setting.initial);
    }
    focusAutomaticCheckbox=CreateWindowExA(0,"BUTTON","Auto focus",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
        0,0,200,24,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(AutoFocusControl)),
        GetModuleHandleA(nullptr),nullptr);
    exposureAutomaticCheckbox=CreateWindowExA(0,"BUTTON","Auto exposure",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
        0,0,200,24,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(AutoExposureControl)),
        GetModuleHandleA(nullptr),nullptr);
    cameraStatusLabel=CreateWindowExA(0,"STATIC","Settings apply to the camera when supported",
        WS_CHILD|WS_VISIBLE,0,0,400,22,window,nullptr,GetModuleHandleA(nullptr),nullptr);
    SendMessageA(focusAutomaticCheckbox,BM_SETCHECK,BST_CHECKED,0);
    SendMessageA(exposureAutomaticCheckbox,BM_SETCHECK,BST_CHECKED,0);
    const char* buttonLabels[]={"Camera / Mask","Presets","Settings","Calibration","Zeroing","Game","Optics / Zoom"};
    for(int index=0;index<7;index++)
        toolbarButtons[index]=CreateWindowExA(0,"BUTTON",buttonLabels[index],WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
        0,0,100,34,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(CameraButtonId+index)),
        GetModuleHandleA(nullptr),nullptr);
    const char* presetLabels[]={"Red Filter","Dark Background","Sensitive","Save User Preset","Load User Preset"};
    for(int index=0;index<5;index++)
        presetButtons[index]=CreateWindowExA(0,"BUTTON",presetLabels[index],WS_CHILD|BS_PUSHBUTTON,
        0,0,250,40,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(PresetOneId+index)),
        GetModuleHandleA(nullptr),nullptr);
    calibrateButton=CreateWindowExA(0,"BUTTON","Select four corners",WS_CHILD|BS_PUSHBUTTON,
        0,0,200,40,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(CalibrateId)),GetModuleHandleA(nullptr),nullptr);
    clearCalibrationButton=CreateWindowExA(0,"BUTTON","Clear calibration",WS_CHILD|BS_PUSHBUTTON,
        0,0,200,40,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(ClearCalibrationId)),GetModuleHandleA(nullptr),nullptr);
    zeroButton=CreateWindowExA(0,"BUTTON","Shoot, then click target",WS_CHILD|BS_PUSHBUTTON,
        0,0,200,40,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(ZeroStartId)),GetModuleHandleA(nullptr),nullptr);
    clearZeroButton=CreateWindowExA(0,"BUTTON","Clear zeroing offset",WS_CHILD|BS_PUSHBUTTON,
        0,0,200,40,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(ZeroClearId)),GetModuleHandleA(nullptr),nullptr);
    offsetXLabel=CreateWindowExA(0,"STATIC","Offset X",WS_CHILD|SS_LEFT,
        0,0,330,22,window,nullptr,GetModuleHandleA(nullptr),nullptr);
    offsetYLabel=CreateWindowExA(0,"STATIC","Offset Y",WS_CHILD|SS_LEFT,
        0,0,330,22,window,nullptr,GetModuleHandleA(nullptr),nullptr);
    offsetXControl=CreateWindowExA(0,TRACKBAR_CLASSA,"",WS_CHILD|TBS_HORZ|TBS_NOTICKS,
        0,0,330,35,window,nullptr,GetModuleHandleA(nullptr),nullptr);
    offsetYControl=CreateWindowExA(0,TRACKBAR_CLASSA,"",WS_CHILD|TBS_HORZ|TBS_NOTICKS,
        0,0,330,35,window,nullptr,GetModuleHandleA(nullptr),nullptr);
    for(HWND control:{offsetXControl,offsetYControl}){
        SendMessageA(control,TBM_SETRANGEMIN,FALSE,-500);
        SendMessageA(control,TBM_SETRANGEMAX,TRUE,500);
    }
    SendMessageA(offsetXControl,TBM_SETPOS,TRUE,static_cast<LPARAM>(std::lround(zeroOffsetX*1000)));
    SendMessageA(offsetYControl,TBM_SETPOS,TRUE,static_cast<LPARAM>(std::lround(zeroOffsetY*1000)));
    updateZeroLabels();
    resolutionLabel=CreateWindowExA(0,"STATIC","Capture resolution (driver may refuse)",WS_CHILD,
        0,0,340,20,window,nullptr,GetModuleHandleA(nullptr),nullptr);
    fpsLabel=CreateWindowExA(0,"STATIC","Requested camera FPS",WS_CHILD,
        0,0,340,20,window,nullptr,GetModuleHandleA(nullptr),nullptr);
    resolutionCombo=CreateWindowExA(0,"COMBOBOX","",WS_CHILD|WS_VSCROLL|CBS_DROPDOWNLIST,
        0,0,340,150,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(CameraResolutionId)),
        GetModuleHandleA(nullptr),nullptr);
    fpsCombo=CreateWindowExA(0,"COMBOBOX","",WS_CHILD|WS_VSCROLL|CBS_DROPDOWNLIST,
        0,0,340,150,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(CameraFpsId)),
        GetModuleHandleA(nullptr),nullptr);
    for(const char* option:{"640 x 480","1280 x 720","1920 x 1080"})
        SendMessageA(resolutionCombo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(option));
    for(const char* option:{"30 FPS","60 FPS","120 FPS"})
        SendMessageA(fpsCombo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(option));
    SendMessageA(resolutionCombo,CB_SETCURSEL,0,0);
    SendMessageA(fpsCombo,CB_SETCURSEL,0,0);
    const char* zoomNames[]={"Digital zoom","Center X","Center Y"};
    const int zoomMinimum[]={100,0,0},zoomMaximum[]={600,1000,1000};
    const int zoomCurrent[]={digitalZoomTimes100,zoomCenterXTimes1000,zoomCenterYTimes1000};
    for(int index=0;index<3;index++){
        zoomLabels[index]=CreateWindowExA(0,"STATIC",zoomNames[index],WS_CHILD|SS_LEFT,
            0,0,340,44,window,nullptr,GetModuleHandleA(nullptr),nullptr);
        zoomTracks[index]=CreateWindowExA(0,TRACKBAR_CLASSA,"",WS_CHILD|TBS_HORZ|TBS_NOTICKS,
            0,0,340,32,window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ZoomTrackId+index)),
            GetModuleHandleA(nullptr),nullptr);
        SendMessageA(zoomTracks[index],TBM_SETRANGEMIN,FALSE,zoomMinimum[index]);
        SendMessageA(zoomTracks[index],TBM_SETRANGEMAX,TRUE,zoomMaximum[index]);
        SendMessageA(zoomTracks[index],TBM_SETPOS,TRUE,zoomCurrent[index]);
    }
    hardwarePropertyLabel=CreateWindowExA(0,"STATIC",
        "Driver camera properties: select one to test",WS_CHILD|SS_LEFT,
        0,0,340,38,window,nullptr,GetModuleHandleA(nullptr),nullptr);
    hardwarePropertyCombo=CreateWindowExA(0,"COMBOBOX","",WS_CHILD|WS_VSCROLL|CBS_DROPDOWNLIST,
        0,0,340,240,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(HardwareComboId)),
        GetModuleHandleA(nullptr),nullptr);
    for(const auto& property:hardwareProperties)
        SendMessageA(hardwarePropertyCombo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(property.name));
    SendMessageA(hardwarePropertyCombo,CB_SETCURSEL,0,0);
    hardwareValueLabel=CreateWindowExA(0,"STATIC","",WS_CHILD|SS_LEFT,0,0,340,42,
        window,nullptr,GetModuleHandleA(nullptr),nullptr);
    hardwareValueTrack=CreateWindowExA(0,TRACKBAR_CLASSA,"",WS_CHILD|TBS_HORZ|TBS_NOTICKS,
        0,0,340,36,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(HardwareValueId)),
        GetModuleHandleA(nullptr),nullptr);
    SendMessageA(hardwareValueTrack,TBM_SETRANGEMIN,FALSE,hardwareProperties[0].minimum);
    SendMessageA(hardwareValueTrack,TBM_SETRANGEMAX,TRUE,hardwareProperties[0].maximum);
    SendMessageA(hardwareValueTrack,TBM_SETPOS,TRUE,hardwareProperties[0].minimum);
    hardwareApplyButton=CreateWindowExA(0,"BUTTON","Apply to camera driver",
        WS_CHILD|BS_PUSHBUTTON,0,0,340,38,window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(HardwareApplyId)),
        GetModuleHandleA(nullptr),nullptr);
    opticsStatus=CreateWindowExA(0,"STATIC","Driver controls are experimental.\n"
        "Zoom may be optical or digital. If a change fails, the camera does not support it.\n"
        "Any zoom change resets calibration.",WS_CHILD|SS_LEFT,
        0,0,340,130,window,nullptr,GetModuleHandleA(nullptr),nullptr);
    updateOpticsLabels();
    settingsStatus=CreateWindowExA(0,"STATIC","",WS_CHILD|SS_LEFT,
        0,0,320,70,window,nullptr,GetModuleHandleA(nullptr),nullptr);
    updateSliderLabels();
    layoutPreviewControls(window);
}


LRESULT CALLBACK previewWindowProcedure(HWND window, UINT message, WPARAM wordParameter, LPARAM longParameter) {
    if(message==WM_LBUTTONDOWN && calibrationStep>=0){
        const int pointX=GET_X_LPARAM(longParameter),pointY=GET_Y_LPARAM(longParameter);
        if(pointX>=cameraViewport.left&&pointX<cameraViewport.right&&
           pointY>=cameraViewport.top&&pointY<cameraViewport.bottom){
            screenCorners[calibrationStep]=cv::Point2f(
                static_cast<float>(pointX-cameraViewport.left)/(cameraViewport.right-cameraViewport.left),
                static_cast<float>(pointY-cameraViewport.top)/(cameraViewport.bottom-cameraViewport.top));
            calibrationStep++;
            cornerCount=calibrationStep;
            if(calibrationStep==4){
                calibrationStep=-1;
                persistCalibration();
                activePanel=CalibrationPanel;
                updatePanels();
            }
        }
        return 0;
    }
    if(message==WM_LBUTTONDOWN && zeroingStep==2 && lastLaserValid){
        const int pointX=GET_X_LPARAM(longParameter),pointY=GET_Y_LPARAM(longParameter);
        if(pointX>=cameraViewport.left&&pointX<cameraViewport.right&&
           pointY>=cameraViewport.top&&pointY<cameraViewport.bottom){
            const cv::Point2f aim(
                static_cast<float>(pointX-cameraViewport.left)/(cameraViewport.right-cameraViewport.left),
                static_cast<float>(pointY-cameraViewport.top)/(cameraViewport.bottom-cameraViewport.top));
            cv::Point2f wanted;
            if(correctedPoint(aim,wanted)){
                zeroOffsetX+=wanted.x-lastLaserCorrected.x;
                zeroOffsetY+=wanted.y-lastLaserCorrected.y;
                persistCalibration();
            }
            zeroingStep=0;activePanel=ZeroPanel;
            if(offsetXControl)SendMessageA(offsetXControl,TBM_SETPOS,TRUE,static_cast<LPARAM>(std::lround(zeroOffsetX*1000)));
            if(offsetYControl)SendMessageA(offsetYControl,TBM_SETPOS,TRUE,static_cast<LPARAM>(std::lround(zeroOffsetY*1000)));
            updateZeroLabels();updatePanels();
        }
        return 0;
    }
    if(message==WM_COMMAND){
        const int clicked=LOWORD(wordParameter);
        if(clicked==CameraButtonId)maskView=!maskView;
        else if(clicked==PresetButtonId)activePanel=activePanel==PresetPanel?NoPanel:PresetPanel;
        else if(clicked==SettingsButtonId)activePanel=activePanel==SettingsPanel?NoPanel:SettingsPanel;
        else if(clicked==CalibrationButtonId)activePanel=activePanel==CalibrationPanel?NoPanel:CalibrationPanel;
        else if(clicked==ZeroButtonId)activePanel=activePanel==ZeroPanel?NoPanel:ZeroPanel;
        else if(clicked==OpticsButtonId)activePanel=activePanel==OpticsPanel?NoPanel:OpticsPanel;
        else if(clicked==GameButtonId){
            ShellExecuteA(window,"open","https://gooodwin67.github.io/laser/game/",
                          nullptr,nullptr,SW_SHOWNORMAL);
        }
        else if(clicked==CalibrateId){cornerCount=0;calibrationStep=0;activePanel=NoPanel;}
        else if(clicked==ClearCalibrationId){cornerCount=0;calibrationStep=-1;persistCalibration();}
        else if(clicked==ZeroStartId){zeroingStep=1;lastLaserValid=false;activePanel=NoPanel;}
        else if(clicked==ZeroClearId){
            zeroOffsetX=0;zeroOffsetY=0;persistCalibration();
            if(offsetXControl)SendMessageA(offsetXControl,TBM_SETPOS,TRUE,0);
            if(offsetYControl)SendMessageA(offsetYControl,TBM_SETPOS,TRUE,0);
            updateZeroLabels();
        }
        else if(clicked>=PresetOneId&&clicked<=PresetLoadId)uiAction=clicked;
        else if(clicked==HardwareComboId&&HIWORD(wordParameter)==CBN_SELCHANGE){
            const int index=static_cast<int>(SendMessageA(hardwarePropertyCombo,CB_GETCURSEL,0,0));
            if(index>=0&&index<hardwarePropertyCount){
                const HardwareProperty& property=hardwareProperties[index];
                SendMessageA(hardwareValueTrack,TBM_SETRANGEMIN,FALSE,property.minimum);
                SendMessageA(hardwareValueTrack,TBM_SETRANGEMAX,TRUE,property.maximum);
                SendMessageA(hardwareValueTrack,TBM_SETPOS,TRUE,property.minimum);
                updateOpticsLabels();
            }
        }
        else if(clicked==HardwareApplyId){
            requestedHardwareProperty=static_cast<int>(SendMessageA(hardwarePropertyCombo,CB_GETCURSEL,0,0));
            requestedHardwareValue=static_cast<int>(SendMessageA(hardwareValueTrack,TBM_GETPOS,0,0));
            hardwareApplyPending=true;
        }
        else if((clicked==CameraResolutionId||clicked==CameraFpsId)&&HIWORD(wordParameter)==CBN_SELCHANGE)
            uiAction=CameraModeApplyId;
        updatePanels();
        return 0;
    }

    if (message==WM_SIZE && focusAutomaticCheckbox){
        updateLayout(window);
        return 0;
    }
    if(message==WM_HSCROLL){
        const HWND changedControl=reinterpret_cast<HWND>(longParameter);
        if(changedControl==zoomTracks[0]||changedControl==zoomTracks[1]||changedControl==zoomTracks[2]){
            const int zoomValue=static_cast<int>(SendMessageA(zoomTracks[0],TBM_GETPOS,0,0));
            const int centerX=static_cast<int>(SendMessageA(zoomTracks[1],TBM_GETPOS,0,0));
            const int centerY=static_cast<int>(SendMessageA(zoomTracks[2],TBM_GETPOS,0,0));
            if(zoomValue!=digitalZoomTimes100||centerX!=zoomCenterXTimes1000||centerY!=zoomCenterYTimes1000){
                digitalZoomTimes100=zoomValue;zoomCenterXTimes1000=centerX;zoomCenterYTimes1000=centerY;
                saveInt("optics","zoom",digitalZoomTimes100);
                saveInt("optics","centerX",zoomCenterXTimes1000);
                saveInt("optics","centerY",zoomCenterYTimes1000);
                resetOpticsCalibration();
                opticsChanged=true;
            }
            updateOpticsLabels();
        }
        if(changedControl==hardwareValueTrack)updateOpticsLabels();
        if(reinterpret_cast<HWND>(longParameter)==offsetXControl ||
           reinterpret_cast<HWND>(longParameter)==offsetYControl){
            zeroOffsetX=static_cast<double>(SendMessageA(offsetXControl,TBM_GETPOS,0,0))/1000;
            zeroOffsetY=static_cast<double>(SendMessageA(offsetYControl,TBM_GETPOS,0,0))/1000;
            persistCalibration();
            updateZeroLabels();
        }
        updateSliderLabels();
        return 0;
    }
    if(message==WM_ERASEBKGND)return 1;
    if(message==WM_PAINT){
        PAINTSTRUCT paint={};
        BeginPaint(window,&paint);
        EndPaint(window,&paint);
        return 0;
    }
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY) {
        running = false;
        previewWindow = nullptr;
        return 0;
    }
    return DefWindowProcA(window, message, wordParameter, longParameter);
}

HWND createPreviewWindow() {
    const char* className = "LaserCoreNativePreview";
    WNDCLASSA windowClass = {};
    windowClass.lpfnWndProc = previewWindowProcedure;
    windowClass.hInstance = GetModuleHandleA(nullptr);
    windowClass.lpszClassName = className;
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    if (!RegisterClassA(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return nullptr;
    }
    HWND window = CreateWindowExA(0, className,
        "Laser Core - Camera / Detection mask (Q to exit)", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1300, 820,
        nullptr, nullptr, windowClass.hInstance, nullptr);
    if (window) {
        createPreviewControls(window);
        ShowWindow(window, SW_SHOW);
        updateLayout(window);
        UpdateWindow(window);
    }
    return window;
}

void drawPreview(const cv::Mat& cameraFrame, const cv::Mat& detectionMask,
                 bool foundLaser, int detectionX, int detectionY) {
    if (!previewWindow || cameraFrame.empty() || detectionMask.empty()) return;
    cv::Mat cameraDisplay;
    cv::Mat maskDisplay;
    cameraFrame.copyTo(cameraDisplay);
    cv::cvtColor(detectionMask, maskDisplay, cv::COLOR_GRAY2BGR);
    if (foundLaser) {
        const cv::Point center(detectionX, detectionY);
        cv::circle(cameraDisplay, center, 13, cv::Scalar(0, 255, 0), 2);
        cv::circle(maskDisplay, center, 13, cv::Scalar(0, 255, 0), 2);
    }
    cv::Mat cameraPixels;
    cv::Mat maskPixels;
    cv::cvtColor(cameraDisplay, cameraPixels, cv::COLOR_BGR2BGRA);
    cv::cvtColor(maskDisplay, maskPixels, cv::COLOR_BGR2BGRA);

    RECT clientRect={};
    GetClientRect(previewWindow,&clientRect);
    const int width=clientRect.right,height=clientRect.bottom;
    if(width<=0||height<=50)return;
    const int drawingWidth=width-(activePanel==NoPanel?0:368);
    const int drawingHeight=height-49;
    if(drawingWidth<=0||drawingHeight<=0)return;
    const double scale=std::min(static_cast<double>(drawingWidth)/cameraPixels.cols,
        static_cast<double>(drawingHeight)/cameraPixels.rows);
    const int imageWidth=std::max(1,static_cast<int>(cameraPixels.cols*scale));
    const int imageHeight=std::max(1,static_cast<int>(cameraPixels.rows*scale));
    const int left=(drawingWidth-imageWidth)/2;
    const int top=49+(drawingHeight-imageHeight)/2;
    cameraViewport={left,top,left+imageWidth,top+imageHeight};
    BITMAPINFO bitmapInfo={};
    bitmapInfo.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth=cameraPixels.cols;
    bitmapInfo.bmiHeader.biHeight=-cameraPixels.rows;
    bitmapInfo.bmiHeader.biPlanes=1;
    bitmapInfo.bmiHeader.biBitCount=32;
    bitmapInfo.bmiHeader.biCompression=BI_RGB;
    HDC windowContext=GetDC(previewWindow);
    if(!windowContext)return;
    HDC deviceContext=CreateCompatibleDC(windowContext);
    if(!deviceContext){ReleaseDC(previewWindow,windowContext);return;}
    HBITMAP backBuffer=CreateCompatibleBitmap(windowContext,width,height);
    if(!backBuffer){DeleteDC(deviceContext);ReleaseDC(previewWindow,windowContext);return;}
    HGDIOBJ oldBitmap=SelectObject(deviceContext,backBuffer);
    // Compose the entire frame off-screen, then present it in ONE BitBlt.
    HBRUSH background=CreateSolidBrush(RGB(7,13,22));
    RECT backgroundRectangle={0,49,drawingWidth,height};
    FillRect(deviceContext,&backgroundRectangle,background);
    DeleteObject(background);
    SetStretchBltMode(deviceContext,HALFTONE);
    const cv::Mat& displayed=maskView?maskPixels:cameraPixels;
    StretchDIBits(deviceContext,left,top,imageWidth,imageHeight,
        0,0,displayed.cols,displayed.rows,displayed.data,&bitmapInfo,DIB_RGB_COLORS,SRCCOPY);
    if(cornerCount>0){
        HPEN cyanPen=CreatePen(PS_SOLID,3,RGB(71,225,245));
        HPEN oldPen=static_cast<HPEN>(SelectObject(deviceContext,cyanPen));
        for(int index=0;index<cornerCount;index++){
            const int x=left+static_cast<int>(screenCorners[index].x*imageWidth);
            const int y=top+static_cast<int>(screenCorners[index].y*imageHeight);
            Ellipse(deviceContext,x-6,y-6,x+6,y+6);
            if(index){
                const int previousX=left+static_cast<int>(screenCorners[index-1].x*imageWidth);
                const int previousY=top+static_cast<int>(screenCorners[index-1].y*imageHeight);
                MoveToEx(deviceContext,previousX,previousY,nullptr);
                LineTo(deviceContext,x,y);
            }
        }
        SelectObject(deviceContext,oldPen);DeleteObject(cyanPen);
    }
    if(activePanel!=NoPanel){
        RECT panelRectangle={drawingWidth,49,width,height};
        HBRUSH panelBrush=CreateSolidBrush(RGB(238,244,251));
        FillRect(deviceContext,&panelRectangle,panelBrush);
        DeleteObject(panelBrush);
    }
    SetBkMode(deviceContext,OPAQUE);
    SetBkColor(deviceContext,RGB(7,13,22));
    SetTextColor(deviceContext,RGB(240,250,255));
    const std::string label=calibrationStep>=0
        ? "CALIBRATE: Click corner "+std::to_string(calibrationStep+1)+"/4 (TL, TR, BR, BL)"
        : zeroingStep==1?"ZEROING: Shoot the target. Several shots are allowed."
        : zeroingStep==2?"ZEROING: Shoot again or click intended aim point (latest shot used)"
        : maskView?"DETECTION MASK - white pixels are candidates"
        :"CAMERA - Presets / Settings / Calibration / Zeroing";
    TextOutA(deviceContext,12,54,label.c_str(),static_cast<int>(label.size()));
    BitBlt(windowContext,0,49,width,height-49,deviceContext,0,49,SRCCOPY);
    SelectObject(deviceContext,oldBitmap);
    DeleteObject(backBuffer);
    DeleteDC(deviceContext);
    ReleaseDC(previewWindow,windowContext);
}



// Generate a short original PCM shot sound once and play it off the camera thread.
const std::vector<char>& shotSoundWav(){
    static const std::vector<char> wave=[]{
        constexpr int sampleRate=22050;
        constexpr int samples=2646;
        std::vector<char> data(44+samples*2,0);
        auto word=[&](int position,unsigned int value,int bytes){
            for(int byteIndex=0;byteIndex<bytes;byteIndex++)
                data[position+byteIndex]=static_cast<char>((value>>(byteIndex*8))&255);
        };
        std::memcpy(data.data(),"RIFF",4);
        word(4,36+samples*2,4);
        std::memcpy(data.data()+8,"WAVEfmt ",8);
        word(16,16,4);word(20,1,2);word(22,1,2);
        word(24,sampleRate,4);word(28,sampleRate*2,4);
        word(32,2,2);word(34,16,2);
        std::memcpy(data.data()+36,"data",4);
        word(40,samples*2,4);
        unsigned int noiseSeed=2463534242u;
        for(int sampleIndex=0;sampleIndex<samples;sampleIndex++){
            const double progress=static_cast<double>(sampleIndex)/samples;
            noiseSeed^=noiseSeed<<13;
            noiseSeed^=noiseSeed>>17;
            noiseSeed^=noiseSeed<<5;
            const double noise=(static_cast<double>(noiseSeed&65535u)/32767.5)-1.0;
            const double oscillation=std::sin(6.28318530718*
                (170.0*sampleIndex/sampleRate-65.0*progress*progress));
            const double envelope=(1-progress)*(1-progress)*(1-progress);
            const double signal=(noise*0.65+oscillation*0.35)*envelope*0.46;
            const short sample=static_cast<short>(std::clamp(signal,-1.0,1.0)*32767);
            word(44+sampleIndex*2,static_cast<unsigned short>(sample),2);
        }
        return data;
    }();
    return wave;
}
void playNativeShot(){
    std::thread([]{
        const auto& sound=shotSoundWav();
        PlaySoundA(sound.data(),nullptr,SND_MEMORY|SND_SYNC|SND_NODEFAULT);
    }).detach();
}

bool sendAll(SOCKET socket, const std::string& data) {
    size_t offset = 0;
    while (offset < data.size()) {
        const int written = send(socket, data.data() + offset,
                                 static_cast<int>(data.size() - offset), 0);
        if (written <= 0) return false;
        offset += static_cast<size_t>(written);
    }
    return true;
}

std::string websocketAcceptKey(const std::string& key) {
    const std::string input = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    BYTE digest[20] = {};
    DWORD digestLength = sizeof(digest);
    std::string result;
    if (CryptAcquireContextA(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        if (CryptCreateHash(provider, CALG_SHA1, 0, 0, &hash) &&
            CryptHashData(hash, reinterpret_cast<const BYTE*>(input.data()),
                          static_cast<DWORD>(input.size()), 0) &&
            CryptGetHashParam(hash, HP_HASHVAL, digest, &digestLength, 0)) {
            DWORD encodedSize = 0;
            const DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
            if (CryptBinaryToStringA(digest, digestLength, flags, nullptr, &encodedSize)) {
                std::vector<char> buffer(encodedSize);
                if (CryptBinaryToStringA(digest, digestLength, flags, buffer.data(), &encodedSize))
                    result.assign(buffer.data());
            }
        }
    }
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    return result;
}

void closeCurrentClient() {
    std::lock_guard<std::mutex> lock(socketMutex);
    if (clientSocket != INVALID_SOCKET) {
        closesocket(clientSocket);
        clientSocket = INVALID_SOCKET;
    }
}

void websocketServer() {
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) return;

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(8765);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(listener, 4) == SOCKET_ERROR) {
        std::cerr << "Cannot listen on 127.0.0.1:8765\n";
        closesocket(listener);
        return;
    }
    std::cout << "WebSocket: ws://127.0.0.1:8765\n";

    while (running) {
        SOCKET incoming = accept(listener, nullptr, nullptr);
        if (incoming == INVALID_SOCKET) break;

        DWORD timeoutMilliseconds = 2500;
        setsockopt(incoming, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeoutMilliseconds),
                   sizeof(timeoutMilliseconds));

        std::string request;
        char buffer[1024];
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192) {
            const int bytesReceived = recv(incoming, buffer, sizeof(buffer), 0);
            if (bytesReceived <= 0) break;
            request.append(buffer, bytesReceived);
        }
        const std::string header = "Sec-WebSocket-Key:";
        const auto headerPosition = request.find(header);
        if (request.find("GET ") != 0 || headerPosition == std::string::npos) {
            closesocket(incoming);
            continue;
        }
        const auto keyStart = request.find_first_not_of(" \t", headerPosition + header.size());
        const auto keyEnd = request.find("\r\n", keyStart);
        if (keyStart == std::string::npos || keyEnd == std::string::npos) {
            closesocket(incoming);
            continue;
        }
        const std::string acceptKey = websocketAcceptKey(request.substr(keyStart, keyEnd - keyStart));
        if (acceptKey.empty()) {
            closesocket(incoming);
            continue;
        }
        const std::string response =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + acceptKey + "\r\n\r\n";
        if (!sendAll(incoming, response)) {
            closesocket(incoming);
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(socketMutex);
            if (clientSocket != INVALID_SOCKET) closesocket(clientSocket);
            clientSocket = incoming;
        }
        std::cout << "Game connected\n";
    }
    closesocket(listener);
}

void sendShot(double normalizedX, double normalizedY, long long timestampMilliseconds) {
    std::ostringstream message;
    message.precision(6);
    message << "{\"type\":\"shot\",\"x\":" << normalizedX
            << ",\"y\":" << normalizedY
            << ",\"timestamp\":" << timestampMilliseconds << "}";
    const std::string payload = message.str();

    // The small shot messages fit the basic (unfragmented) WebSocket text frame.
    if (payload.size() >= 126) return;
    std::string frame;
    frame.push_back(static_cast<char>(0x81));
    frame.push_back(static_cast<char>(payload.size()));
    frame += payload;

    std::lock_guard<std::mutex> lock(socketMutex);
    if (clientSocket != INVALID_SOCKET && !sendAll(clientSocket, frame)) {
        closesocket(clientSocket);
        clientSocket = INVALID_SOCKET;
    }
}

int argumentValue(int argc, char** argv, int index, int defaultValue) {
    if (argc <= index) return defaultValue;
    try { return std::stoi(argv[index]); }
    catch (...) { return defaultValue; }
}
}

int main(int argc, char** argv) {
    // laser-core.exe [camera-index] [capture-width] [capture-height]
    //                [requested-fps] [brightness] [red-difference]
    const int cameraIndex = argumentValue(argc, argv, 1, 0);
    const int captureWidth = argumentValue(argc, argv, 2, 640);
    const int captureHeight = argumentValue(argc, argv, 3, 480);
    const int requestedFps = argumentValue(argc, argv, 4, 30);
    const int initialBrightness = argumentValue(argc, argv, 5, 190);
    const int initialRedDifference = argumentValue(argc, argv, 6, 45);
    const int initialSpotPixels = std::max(1, argumentValue(argc, argv, 7, 4));
    char executablePath[MAX_PATH]={};
    GetModuleFileNameA(nullptr,executablePath,MAX_PATH);
    std::string applicationPath(executablePath);
    const size_t separator=applicationPath.find_last_of("\\\\/");
    settingsIniPath=applicationPath.substr(0,separator+1)+"laser-core.ini";
    loadCalibration();

    WSADATA winsockData = {};
    if (WSAStartup(MAKEWORD(2, 2), &winsockData) != 0) {
        std::cerr << "Winsock initialization failed\n";
        return 1;
    }

    cv::VideoCapture camera(cameraIndex, cv::CAP_DSHOW);
    if (!camera.isOpened()) {
        std::cerr << "Cannot open USB camera " << cameraIndex << "\n";
        WSACleanup();
        return 1;
    }
    const int preferredWidth=iniInt("camera","width",captureWidth);
    const int preferredHeight=iniInt("camera","height",captureHeight);
    const int preferredFps=iniInt("camera","fps",requestedFps);
    camera.set(cv::CAP_PROP_FRAME_WIDTH,preferredWidth);
    camera.set(cv::CAP_PROP_FRAME_HEIGHT,preferredHeight);
    camera.set(cv::CAP_PROP_FPS,preferredFps);
    camera.set(cv::CAP_PROP_BUFFERSIZE, 1);
    std::cout << "Camera actual settings: "
              << camera.get(cv::CAP_PROP_FRAME_WIDTH) << "x"
              << camera.get(cv::CAP_PROP_FRAME_HEIGHT) << " at "
              << camera.get(cv::CAP_PROP_FPS) << " fps (driver-reported)\n";
    std::cout << "Detector: minimum red=" << initialBrightness
              << ", red-difference=" << initialRedDifference << "\n";

    previewWindow = createPreviewWindow();
    if (!previewWindow) std::cerr << "Camera preview unavailable; detection will continue.\n";
    if(previewWindow){
        const int resolutionIndex=preferredWidth>=1900?2:preferredWidth>=1200?1:0;
        const int fpsIndex=preferredFps>=120?2:preferredFps>=60?1:0;
        SendMessageA(resolutionCombo,CB_SETCURSEL,resolutionIndex,0);
        SendMessageA(fpsCombo,CB_SETCURSEL,fpsIndex,0);
        const int initialValues[]={
            iniInt("settings","Brightness",initialBrightness),
            iniInt("settings","Red excess",initialRedDifference),
            iniInt("settings","Red change",28),
            iniInt("settings","Spot pixels",initialSpotPixels)};
        for(int index=0;index<4;index++)
            SendMessageA(sliders[index].trackbar,TBM_SETPOS,TRUE,initialValues[index]);
        // DirectShow camera properties are allowed to report unsupported values.
        const double currentFocus=camera.get(cv::CAP_PROP_FOCUS);
        const double currentExposure=camera.get(cv::CAP_PROP_EXPOSURE);
        if(std::isfinite(currentFocus)&&currentFocus>=0&&currentFocus<=1023)
            SendMessageA(sliders[4].trackbar,TBM_SETPOS,TRUE,static_cast<int>(currentFocus));
        if(std::isfinite(currentExposure)&&currentExposure>=-13&&currentExposure<=0)
            SendMessageA(sliders[5].trackbar,TBM_SETPOS,TRUE,static_cast<int>(currentExposure));
        updateSliderLabels();
    }

    std::thread serverThread(websocketServer);
    serverThread.detach(); // Process lifetime; accept loop ends when the process exits.

    cv::Mat frame, red, green, blue, otherMaximum, redExcess;
    cv::Mat brightMask, redMask, colorMask, previousRed, redRise, riseMask, onsetMask;
    cv::Mat labels, stats, centroids;
    long long captureCount = 0;
    long long totalFrames = 0;
    long long shotCount = 0;
    auto statisticsStart = std::chrono::steady_clock::now();
    auto lastShotTime = statisticsStart - std::chrono::seconds(1);
    int lastCameraFocus=-9999;
    int lastCameraExposure=-9999;
    bool lastAutomaticFocus=true;
    bool lastAutomaticExposure=true;

    constexpr int maximumSpotPixels = 160;
    constexpr int minimumIntervalMilliseconds = 220;

    std::cout << "Onset detector: red rise >= " << (previewWindow?sliderValue(RedRiseControl):28)
              << " in ONE captured frame; 220 ms debounce.\n";
    std::cout << "Camera preview and detection mask with controls for focus, exposure, and detector.\n";
    std::cout << "Keep camera fixed. Press Q in this console or close the preview window.\n";
    int previousSettingValues[6]={-9999,-9999,-9999,-9999,-9999,-9999};
    while (running) {
        const int action=uiAction.exchange(0);
        if(previewWindow&&action==CameraModeApplyId){
            const int resolutionIndex=static_cast<int>(SendMessageA(resolutionCombo,CB_GETCURSEL,0,0));
            const int fpsIndex=static_cast<int>(SendMessageA(fpsCombo,CB_GETCURSEL,0,0));
            const int widths[]={640,1280,1920},heights[]={480,720,1080},fpsOptions[]={30,60,120};
            if(resolutionIndex>=0&&resolutionIndex<3&&fpsIndex>=0&&fpsIndex<3){
                const bool widthApplied=camera.set(cv::CAP_PROP_FRAME_WIDTH,widths[resolutionIndex]);
                const bool heightApplied=camera.set(cv::CAP_PROP_FRAME_HEIGHT,heights[resolutionIndex]);
                const bool fpsApplied=camera.set(cv::CAP_PROP_FPS,fpsOptions[fpsIndex]);
                previousRed.release();
                saveInt("camera","width",widths[resolutionIndex]);
                saveInt("camera","height",heights[resolutionIndex]);
                saveInt("camera","fps",fpsOptions[fpsIndex]);
                std::cout<<"Requested camera mode "<<widths[resolutionIndex]<<"x"<<heights[resolutionIndex]
                         <<" "<<fpsOptions[fpsIndex]<<"fps; driver applied W/H/FPS="
                         <<widthApplied<<"/"<<heightApplied<<"/"<<fpsApplied<<"\n";
            }
        }
        if(previewWindow&&action>=PresetOneId&&action<=PresetLoadId){
            const int index=action-PresetOneId;
            if(index==3){
                for(int settingIndex=0;settingIndex<6;settingIndex++)
                    saveInt("preset-user",sliders[settingIndex].caption,sliderValue(sliders[settingIndex].id));
                saveInt("preset-user","saved",1);
            }else if(index==4){
                if(iniInt("preset-user","saved",0)){
                    for(int settingIndex=0;settingIndex<6;settingIndex++){
                        const int value=iniInt("preset-user",sliders[settingIndex].caption,sliders[settingIndex].initial);
                        SendMessageA(sliders[settingIndex].trackbar,TBM_SETPOS,TRUE,value);
                    }
                }
            }else{
                const int presets[3][4]={{141,136,46,1},{205,40,20,2},{135,40,16,1}};
                for(int settingIndex=0;settingIndex<4;settingIndex++)
                    SendMessageA(sliders[settingIndex].trackbar,TBM_SETPOS,TRUE,presets[index][settingIndex]);
            }
            updateSliderLabels();
        }
        if(previewWindow){
            for(int settingIndex=0;settingIndex<6;settingIndex++){
                const int value=sliderValue(sliders[settingIndex].id);
                if(value!=previousSettingValues[settingIndex]){
                    saveInt("settings",sliders[settingIndex].caption,value);
                    previousSettingValues[settingIndex]=value;
                }
            }
        }
        const int minimumBrightness=previewWindow?sliderValue(BrightnessControl):initialBrightness;
        const int minimumRedDifference=previewWindow?sliderValue(RedDifferenceControl):initialRedDifference;
        const int redRiseThreshold=previewWindow?sliderValue(RedRiseControl):28;
        const int minimumSpotPixels=previewWindow?sliderValue(SpotAreaControl):initialSpotPixels;
        if(previewWindow){
            const bool autoFocus=checkboxChecked(focusAutomaticCheckbox);
            const bool autoExposure=checkboxChecked(exposureAutomaticCheckbox);
            const int requestedFocus=sliderValue(FocusControl);
            const int requestedExposure=sliderValue(ExposureControl);
            if(autoFocus!=lastAutomaticFocus){
                camera.set(cv::CAP_PROP_AUTOFOCUS,autoFocus?1:0);
                lastAutomaticFocus=autoFocus;
                lastCameraFocus=-9999;
            }
            if(!autoFocus&&requestedFocus!=lastCameraFocus){
                const bool changed=camera.set(cv::CAP_PROP_FOCUS,requestedFocus);
                if(!changed)std::cerr<<"Driver refused manual focus "<<requestedFocus<<"\n";
                lastCameraFocus=requestedFocus;
            }
            if(autoExposure!=lastAutomaticExposure){
                camera.set(cv::CAP_PROP_AUTO_EXPOSURE,autoExposure?0.75:0.25);
                lastAutomaticExposure=autoExposure;
                lastCameraExposure=-9999;
            }
            if(!autoExposure&&requestedExposure!=lastCameraExposure){
                const bool changed=camera.set(cv::CAP_PROP_EXPOSURE,requestedExposure);
                if(!changed)std::cerr<<"Driver refused exposure "<<requestedExposure<<"\n";
                lastCameraExposure=requestedExposure;
            }
        }
        const auto readStart = std::chrono::steady_clock::now();
        if (!camera.read(frame) || frame.empty()) {
            std::cerr << "Camera read failed\n";
            break;
        }
        const auto capturedAt = std::chrono::steady_clock::now();
        const double readMilliseconds =
            std::chrono::duration<double, std::milli>(capturedAt - readStart).count();
        captureCount++;
        totalFrames++;

        cv::extractChannel(frame, blue, 0);
        cv::extractChannel(frame, green, 1);
        cv::extractChannel(frame, red, 2);
        cv::max(blue, green, otherMaximum);
        cv::subtract(red, otherMaximum, redExcess);
        cv::threshold(red, brightMask, minimumBrightness - 1, 255, cv::THRESH_BINARY);
        cv::threshold(redExcess, redMask, minimumRedDifference - 1, 255, cv::THRESH_BINARY);
        cv::bitwise_and(brightMask, redMask, colorMask);

        bool detectedOnset = false;
        int risingPixelCount = 0;
        int bestArea = 0;
        int strongestRise = 0;
        int detectedX = -1;
        int detectedY = -1;
        if (!previousRed.empty()) {
            // Difference from the preceding CAPTURED frame. A fixed red object
            // cannot remain a candidate merely because of a frozen background.
            cv::subtract(red, previousRed, redRise);
            cv::threshold(redRise, riseMask, redRiseThreshold - 1, 255, cv::THRESH_BINARY);
            cv::bitwise_and(colorMask, riseMask, onsetMask);
            risingPixelCount = cv::countNonZero(onsetMask);

            // Reject global exposure changes as they usually brighten hundreds
            // or thousands of pixels simultaneously.
            const int maximumGlobalChangedPixels = std::max(120, frame.rows * frame.cols / 200);
            if (risingPixelCount > 0 && risingPixelCount <= maximumGlobalChangedPixels) {
                const int regions = cv::connectedComponentsWithStats(
                    onsetMask, labels, stats, centroids, 8);
                int bestRegion = -1;
                for (int region = 1; region < regions; region++) {
                    const int area = stats.at<int>(region, cv::CC_STAT_AREA);
                    if (area >= minimumSpotPixels && area <= maximumSpotPixels && area > bestArea) {
                        bestRegion = region;
                        bestArea = area;
                    }
                }
                if (bestRegion >= 0) {
                    detectedOnset = true;
                    const auto millisecondsSinceShot =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            capturedAt - lastShotTime).count();
                    const int regionX = static_cast<int>(centroids.at<double>(bestRegion, 0));
                    const int regionY = static_cast<int>(centroids.at<double>(bestRegion, 1));
                    strongestRise = static_cast<int>(redRise.at<unsigned char>(regionY, regionX));
                    detectedX = regionX;
                    detectedY = regionY;
                    if (millisecondsSinceShot >= minimumIntervalMilliseconds) {
                        const double pointX =
                            (centroids.at<double>(bestRegion, 0) + 0.5) / frame.cols;
                        const double pointY =
                            (centroids.at<double>(bestRegion, 1) + 0.5) / frame.rows;
                        const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                            capturedAt.time_since_epoch()).count();
                        const cv::Point2f rawPoint(
                            static_cast<float>(pointX),static_cast<float>(pointY));
                        cv::Point2f mappedPoint;
                        if(!correctedPoint(rawPoint,mappedPoint))continue;
                        sendShot(mappedPoint.x,mappedPoint.y,timestamp);
                        playNativeShot();
                        lastLaserRaw=rawPoint;
                        lastLaserCorrected=mappedPoint;
                        lastLaserValid=true;
                        if(zeroingStep==1||zeroingStep==2)zeroingStep=2;
                        shotCount++;
                        lastShotTime = capturedAt;
                        std::cout << "Shot " << shotCount
                                  << " frame=" << totalFrames
                                  << " at (" << pointX << ", " << pointY << ")"
                                  << " area=" << bestArea
                                  << " rise=" << strongestRise
                                  << " read=" << readMilliseconds << "ms\n";
                    }
                }
            }
        }
        // Show the current camera frame and the exact detector mask.
        // Preview refreshes at capture speed; measure any FPS impact in diagnostics.
        if(!onsetMask.empty())drawPreview(frame,onsetMask,detectedOnset,detectedX,detectedY);
        MSG windowMessage = {};
        while (PeekMessageA(&windowMessage, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&windowMessage);
            DispatchMessageA(&windowMessage);
        }
        if (!running) break;

        // ALWAYS update the reference frame, including during an extended
        // illumination. Thus holding a laser steady will not fire repeatedly.
        red.copyTo(previousRed);

        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - statisticsStart).count();
        if (elapsed >= 3.0) {
            if(previewWindow&&cameraStatusLabel){
                std::ostringstream actualSettings;
                actualSettings << "Actual focus: " << camera.get(cv::CAP_PROP_FOCUS)
                               << " | exposure: " << camera.get(cv::CAP_PROP_EXPOSURE)
                               << " | FPS: " << camera.get(cv::CAP_PROP_FPS);
                actualSettings << " | calibration " << cornerCount << "/4"
                               << " | offset " << zeroOffsetX << ", " << zeroOffsetY;
                SetWindowTextA(cameraStatusLabel,actualSettings.str().c_str());
            }
            std::cout << "Captured FPS: " << (captureCount / elapsed)
                      << " | onset: " << (detectedOnset ? "YES" : "no")
                      << " | rising pixels: " << risingPixelCount
                      << " | read: " << readMilliseconds << "ms"
                      << " | shots: " << shotCount << "\n";
            captureCount = 0;
            statisticsStart = std::chrono::steady_clock::now();
        }

        if (_kbhit()) {
            const int pressedKey = _getch();
            if (pressedKey == 'q' || pressedKey == 'Q') break;
        }
    }
    running = false;
    if (previewWindow) DestroyWindow(previewWindow);
    closeCurrentClient();
    camera.release();
    WSACleanup();
    return 0;
}
