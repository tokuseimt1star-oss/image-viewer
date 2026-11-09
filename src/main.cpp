#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <commdlg.h>
#include <cstring>
#include <wincodec.h>
#include <d2d1.h>
#include <wrl/client.h>
#include <archive.h>
#include <archive_entry.h>
#include <webp/decode.h>
#include <avif/avif.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <cwctype>
#include <cmath>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
static constexpr uint64_t MAX_IMAGE_BYTES = 256ull * 1024 * 1024;
static constexpr uint64_t MAX_PIXELS = 100000000ull;

struct Image { UINT w=0,h=0; std::vector<uint8_t> bgra; };
static HWND g_hwnd{};
static ComPtr<ID2D1Factory> g_factory;
static ComPtr<ID2D1HwndRenderTarget> g_target;
static ComPtr<ID2D1Bitmap> g_bitmap;
static Image g_image;
static std::vector<std::wstring> g_items;
static size_t g_index=0;
static std::wstring g_source, g_archive;
static bool g_isArchive=false, g_fullscreen=false;
static RECT g_oldRect{}; static DWORD g_oldStyle{};
static float g_zoom=1.0f;
static bool g_fit=true;
static constexpr UINT ID_REGISTER_SHELL=41001, ID_UNREGISTER_SHELL=41002;

static std::wstring lower(std::wstring s){ std::transform(s.begin(),s.end(),s.begin(),[](wchar_t c){return (wchar_t)towlower(c);}); return s; }
static bool imageExt(const std::wstring& p){ auto e=lower(fs::path(p).extension().wstring()); return e==L".jpg"||e==L".jpeg"||e==L".png"||e==L".bmp"||e==L".gif"||e==L".tif"||e==L".tiff"||e==L".webp"||e==L".avif"; }
static bool readFile(const std::wstring& p,std::vector<uint8_t>& b){ std::ifstream f(fs::path(p),std::ios::binary|std::ios::ate); if(!f)return false; auto n=f.tellg(); if(n<0||(uint64_t)n>MAX_IMAGE_BYTES)return false; b.resize((size_t)n); f.seekg(0); return b.empty()||bool(f.read((char*)b.data(),n)); }
static bool decodeWic(const uint8_t* data,size_t size,Image& out){
 if(size>UINT_MAX)return false; ComPtr<IWICImagingFactory> fac; if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&fac))))return false;
 ComPtr<IWICStream> stream; if(FAILED(fac->CreateStream(&stream)))return false;
 if(FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(data),(DWORD)size)))return false;
 ComPtr<IWICBitmapDecoder> dec; if(FAILED(fac->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnLoad,&dec)))return false;
 ComPtr<IWICBitmapFrameDecode> frame; if(FAILED(dec->GetFrame(0,&frame)))return false;
 UINT w=0,h=0; if(FAILED(frame->GetSize(&w,&h))||!w||!h||(uint64_t)w*h>MAX_PIXELS)return false;
 ComPtr<IWICFormatConverter> cv; if(FAILED(fac->CreateFormatConverter(&cv)))return false;
 if(FAILED(cv->Initialize(frame.Get(),GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)))return false;
 out.w=w;out.h=h;out.bgra.resize((size_t)w*h*4); return SUCCEEDED(cv->CopyPixels(nullptr,w*4,(UINT)out.bgra.size(),out.bgra.data()));
}
static bool decodeBytes(const std::vector<uint8_t>& b,const std::wstring& name,Image& out){
 if(b.empty()||b.size()>MAX_IMAGE_BYTES)return false; auto e=lower(fs::path(name).extension().wstring());
 if(e==L".webp") { int w=0,h=0; uint8_t* p=WebPDecodeBGRA(b.data(),b.size(),&w,&h); if(!p||w<=0||h<=0||(uint64_t)w*h>MAX_PIXELS){if(p)WebPFree(p);return false;} out.w=(UINT)w;out.h=(UINT)h;out.bgra.assign(p,p+(size_t)w*h*4);WebPFree(p); for(size_t i=0;i<out.bgra.size();i+=4){unsigned a=out.bgra[i+3];out.bgra[i]=(uint8_t)(out.bgra[i]*a/255);out.bgra[i+1]=(uint8_t)(out.bgra[i+1]*a/255);out.bgra[i+2]=(uint8_t)(out.bgra[i+2]*a/255);} return true; }
 if(e==L".avif") { avifDecoder* d=avifDecoderCreate(); if(!d)return false; avifResult r=avifDecoderReadMemory(d,nullptr,b.data(),b.size()); if(r!=AVIF_RESULT_OK||!d->image||(uint64_t)d->image->width*d->image->height>MAX_PIXELS){avifDecoderDestroy(d);return false;} avifRGBImage rgb; avifRGBImageSetDefaults(&rgb,d->image); rgb.format=AVIF_RGB_FORMAT_BGRA; rgb.depth=8; rgb.pixels=nullptr; avifRGBImageAllocatePixels(&rgb); r=avifImageYUVToRGB(d->image,&rgb); bool ok=r==AVIF_RESULT_OK; if(ok){out.w=d->image->width;out.h=d->image->height;out.bgra.resize((size_t)rgb.rowBytes*rgb.height); for(uint32_t y=0;y<rgb.height;y++)memcpy(out.bgra.data()+(size_t)y*rgb.width*4,rgb.pixels+(size_t)y*rgb.rowBytes,(size_t)rgb.width*4); for(size_t i=0;i<out.bgra.size();i+=4){unsigned a=out.bgra[i+3];out.bgra[i]=(uint8_t)(out.bgra[i]*a/255);out.bgra[i+1]=(uint8_t)(out.bgra[i+1]*a/255);out.bgra[i+2]=(uint8_t)(out.bgra[i+2]*a/255);}} avifRGBImageFreePixels(&rgb);avifDecoderDestroy(d);return ok; }
 return decodeWic(b.data(),b.size(),out);
}
static bool readArchiveEntry(const std::wstring& archivePath,const std::wstring& wanted,std::vector<uint8_t>& bytes){
 struct archive* a=archive_read_new(); if(!a)return false; archive_read_support_format_zip(a); archive_read_support_filter_all(a); std::string path=fs::path(archivePath).u8string(); if(archive_read_open_filename(a,path.c_str(),10240)!=ARCHIVE_OK){archive_read_free(a);return false;}
 struct archive_entry* ent=nullptr; bool ok=false; while(archive_read_next_header(a,&ent)==ARCHIVE_OK){ const char* pn=archive_entry_pathname_utf8(ent); if(!pn)pn=archive_entry_pathname(ent); if(!pn){archive_read_data_skip(a);continue;} int n=MultiByteToWideChar(CP_UTF8,0,pn,-1,nullptr,0); std::wstring name; if(n>0){name.resize(n);MultiByteToWideChar(CP_UTF8,0,pn,-1,name.data(),n);name.pop_back();} if(name==wanted){ la_int64_t sz=archive_entry_size(ent); if(sz>=0&&(uint64_t)sz<=MAX_IMAGE_BYTES){bytes.resize((size_t)sz); size_t off=0; while(off<bytes.size()){la_ssize_t got=archive_read_data(a,bytes.data()+off,bytes.size()-off);if(got<=0)break;off+=(size_t)got;} ok=off==bytes.size();} break;} archive_read_data_skip(a); }
 archive_read_close(a);archive_read_free(a);return ok;
}
static bool scanArchive(const std::wstring& p){ g_items.clear(); struct archive* a=archive_read_new(); if(!a)return false; archive_read_support_format_zip(a);archive_read_support_filter_all(a); std::string u=fs::path(p).u8string(); if(archive_read_open_filename(a,u.c_str(),10240)!=ARCHIVE_OK){archive_read_free(a);return false;} struct archive_entry* ent=nullptr; while(archive_read_next_header(a,&ent)==ARCHIVE_OK){ if(archive_entry_filetype(ent)==AE_IFREG){const char* pn=archive_entry_pathname_utf8(ent);if(!pn)pn=archive_entry_pathname(ent);if(pn){int n=MultiByteToWideChar(CP_UTF8,0,pn,-1,nullptr,0);if(n>1){std::wstring name(n,L'\0');MultiByteToWideChar(CP_UTF8,0,pn,-1,name.data(),n);name.pop_back();if(imageExt(name))g_items.push_back(name);}}}archive_read_data_skip(a);} archive_read_close(a);archive_read_free(a);std::sort(g_items.begin(),g_items.end(),[](auto&a,auto&b){return lower(a)<lower(b);});return !g_items.empty(); }
static bool scanFolder(const std::wstring& p,const std::wstring& first={}){g_items.clear();try{for(auto& x:fs::directory_iterator(fs::path(p)))if(x.is_regular_file()&&imageExt(x.path().wstring()))g_items.push_back(x.path().wstring());}catch(...){return false;}std::sort(g_items.begin(),g_items.end(),[](auto&a,auto&b){return lower(fs::path(a).filename().wstring())<lower(fs::path(b).filename().wstring());});if(!first.empty()){auto it=std::find(g_items.begin(),g_items.end(),first);if(it!=g_items.end())g_index=(size_t)(it-g_items.begin());}return !g_items.empty();}
static void setTitle(){std::wstring t=L"MassiGra Modern";if(!g_items.empty())t+=L"  -  "+(g_isArchive?fs::path(g_archive).filename().wstring():fs::path(g_items[g_index]).filename().wstring())+L"  ["+std::to_wstring(g_index+1)+L"/"+std::to_wstring(g_items.size())+L"]";SetWindowTextW(g_hwnd,t.c_str());}
static void createBitmap(){g_bitmap.Reset();if(!g_target||g_image.bgra.empty())return;D2D1_BITMAP_PROPERTIES bp=D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED));g_target->CreateBitmap(D2D1::SizeU(g_image.w,g_image.h),g_image.bgra.data(),g_image.w*4,&bp,&g_bitmap);}
static bool loadCurrent(){if(g_items.empty())return false;std::vector<uint8_t>b;bool r=g_isArchive?readArchiveEntry(g_archive,g_items[g_index],b):readFile(g_items[g_index],b);if(!r||!decodeBytes(b,g_items[g_index],g_image))return false;createBitmap();setTitle();InvalidateRect(g_hwnd,nullptr,FALSE);return true;}
static void movePage(int d){if(g_items.empty())return;int64_t n=(int64_t)g_index+d;if(n<0)n=0;if(n>=(int64_t)g_items.size())n=(int64_t)g_items.size()-1;if((size_t)n!=g_index){g_index=(size_t)n;g_zoom=1;g_fit=true;if(!loadCurrent())MessageBeep(MB_ICONWARNING);}}
static void toggleFullscreen(){if(!g_fullscreen){g_oldStyle=(DWORD)GetWindowLongPtrW(g_hwnd,GWL_STYLE);GetWindowRect(g_hwnd,&g_oldRect);MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromWindow(g_hwnd,MONITOR_DEFAULTTONEAREST),&mi);SetWindowLongPtrW(g_hwnd,GWL_STYLE,g_oldStyle&~(WS_CAPTION|WS_THICKFRAME));SetWindowPos(g_hwnd,HWND_TOP,mi.rcMonitor.left,mi.rcMonitor.top,mi.rcMonitor.right-mi.rcMonitor.left,mi.rcMonitor.bottom-mi.rcMonitor.top,SWP_FRAMECHANGED);g_fullscreen=true;}else{SetWindowLongPtrW(g_hwnd,GWL_STYLE,g_oldStyle);SetWindowPos(g_hwnd,nullptr,g_oldRect.left,g_oldRect.top,g_oldRect.right-g_oldRect.left,g_oldRect.bottom-g_oldRect.top,SWP_NOZORDER|SWP_FRAMECHANGED);g_fullscreen=false;} }
static void render(){if(!g_target)return;g_target->BeginDraw();g_target->Clear(D2D1::ColorF(0.075f,0.075f,0.075f));if(g_bitmap){D2D1_SIZE_F s=g_target->GetSize();float w=(float)g_image.w,h=(float)g_image.h;float z=g_fit?std::min(s.width/w,s.height/h):g_zoom;w*=z;h*=z;D2D1_RECT_F r=D2D1::RectF((s.width-w)/2,(s.height-h)/2,(s.width+w)/2,(s.height+h)/2);g_target->DrawBitmap(g_bitmap.Get(),r,1.0f,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);}if(FAILED(g_target->EndDraw())){g_target.Reset();g_bitmap.Reset();}}
static void ensureTarget(){if(g_target)return;RECT r{};GetClientRect(g_hwnd,&r);g_factory->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),D2D1::HwndRenderTargetProperties(g_hwnd,D2D1::SizeU(r.right,r.bottom)),&g_target);createBitmap();}
static void addVerb(HKEY root,const std::wstring& sub,const std::wstring& exe){HKEY k{};if(RegCreateKeyExW(root,sub.c_str(),0,nullptr,0,KEY_WRITE,nullptr,&k,nullptr)!=ERROR_SUCCESS)return;const wchar_t* label=L"MassiGra Modernで開く";RegSetValueExW(k,nullptr,0,REG_SZ,(const BYTE*)label,(DWORD)((wcslen(label)+1)*sizeof(wchar_t)));HKEY c{};if(RegCreateKeyExW(k,L"command",0,nullptr,0,KEY_WRITE,nullptr,&c,nullptr)==ERROR_SUCCESS){std::wstring cmd=L"\""+exe+L"\" \"%1\"";RegSetValueExW(c,nullptr,0,REG_SZ,(const BYTE*)cmd.c_str(),(DWORD)((cmd.size()+1)*sizeof(wchar_t)));RegCloseKey(c);}RegCloseKey(k);}
static void registerShell(bool on){wchar_t exe[MAX_PATH]{};GetModuleFileNameW(nullptr,exe,MAX_PATH);std::wstring base=L"Software\\Classes\\";std::vector<std::wstring> keys;for(auto e:{L".jpg",L".jpeg",L".png",L".bmp",L".gif",L".tif",L".tiff",L".webp",L".avif",L".zip",L".cbz"})keys.push_back(base+L"SystemFileAssociations\\"+e+L"\\shell\\MassiGraModern");keys.push_back(base+L"Directory\\shell\\MassiGraModern");for(auto&k:keys){if(on)addVerb(HKEY_CURRENT_USER,k,exe);else RegDeleteTreeW(HKEY_CURRENT_USER,k.c_str());}SHChangeNotify(SHCNE_ASSOCCHANGED,SHCNF_IDLIST,nullptr,nullptr);}
static void openPath(const std::wstring& p){g_source=p;g_index=0;g_isArchive=false;g_archive.clear();auto ext=lower(fs::path(p).extension().wstring());if(ext==L".zip"||ext==L".cbz"){g_isArchive=true;g_archive=p;if(!scanArchive(p)){MessageBoxW(g_hwnd,L"書庫を開けないか、対応画像がありません。",L"MassiGra Modern",MB_ICONERROR);return;}}else if(fs::is_directory(fs::path(p))){if(!scanFolder(p)){MessageBoxW(g_hwnd,L"フォルダ内に対応画像がありません。",L"MassiGra Modern",MB_ICONINFORMATION);return;}}else{auto dir=fs::path(p).parent_path().wstring();if(!scanFolder(dir,p)){g_items={p};}}loadCurrent();}
static LRESULT CALLBACK wndProc(HWND h,UINT m,WPARAM w,LPARAM l){switch(m){case WM_SIZE:if(g_target)g_target->Resize(D2D1::SizeU(LOWORD(l),HIWORD(l)));InvalidateRect(h,nullptr,FALSE);return 0;case WM_PAINT:{PAINTSTRUCT ps;BeginPaint(h,&ps);ensureTarget();render();EndPaint(h,&ps);return 0;}case WM_MOUSEWHEEL:if(!(GetKeyState(VK_CONTROL)&0x8000))movePage(GET_WHEEL_DELTA_WPARAM(w)<0?1:-1);else{g_fit=false;g_zoom*=GET_WHEEL_DELTA_WPARAM(w)>0?1.15f:0.87f;InvalidateRect(h,nullptr,FALSE);}return 0;case WM_COMMAND: if(LOWORD(w)==ID_REGISTER_SHELL){registerShell(true);MessageBoxW(h,L"右クリックメニューを登録しました。",L"MassiGra Modern",MB_ICONINFORMATION);return 0;} if(LOWORD(w)==ID_UNREGISTER_SHELL){registerShell(false);MessageBoxW(h,L"右クリックメニューを解除しました。",L"MassiGra Modern",MB_ICONINFORMATION);return 0;} return 0;case WM_KEYDOWN:switch(w){case VK_RIGHT:case VK_SPACE:movePage(1);break;case VK_LEFT:case VK_BACK:movePage(-1);break;case VK_ESCAPE:if(g_fullscreen)toggleFullscreen();else DestroyWindow(h);break;case VK_RETURN:case VK_F11:toggleFullscreen();break;case 'F':g_fit=true;InvalidateRect(h,nullptr,FALSE);break;case '1':g_fit=false;g_zoom=1;InvalidateRect(h,nullptr,FALSE);break;case 'O':if(GetKeyState(VK_CONTROL)&0x8000){wchar_t f[MAX_PATH]{};OPENFILENAMEW of{sizeof(of)};of.hwndOwner=h;of.lpstrFile=f;of.nMaxFile=MAX_PATH;of.lpstrFilter=L"Images and archives\0*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.tif;*.tiff;*.webp;*.avif;*.zip;*.cbz\0All files\0*.*\0";of.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST;if(GetOpenFileNameW(&of))openPath(f);}break;}return 0;case WM_DROPFILES:{HDROP d=(HDROP)w;wchar_t p[MAX_PATH]{};if(DragQueryFileW(d,0,p,MAX_PATH))openPath(p);DragFinish(d);return 0;}case WM_DESTROY:PostQuitMessage(0);return 0;}return DefWindowProcW(h,m,w,l);}
int WINAPI wWinMain(HINSTANCE inst,HINSTANCE, PWSTR cmd,int show){CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,&g_factory);WNDCLASSW wc{};wc.lpfnWndProc=wndProc;wc.hInstance=inst;wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.lpszClassName=L"MassiGraModernWindow";RegisterClassW(&wc);g_hwnd=CreateWindowExW(WS_EX_ACCEPTFILES,wc.lpszClassName,L"MassiGra Modern",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,1000,760,nullptr,nullptr,inst,nullptr);HMENU bar=CreateMenu();HMENU settings=CreatePopupMenu();AppendMenuW(settings,MF_STRING,ID_REGISTER_SHELL,L"右クリックメニューを登録");AppendMenuW(settings,MF_STRING,ID_UNREGISTER_SHELL,L"右クリックメニューを解除");AppendMenuW(bar,MF_POPUP,(UINT_PTR)settings,L"設定");SetMenu(g_hwnd,bar);ShowWindow(g_hwnd,show);if(cmd&&*cmd){std::wstring p=cmd;if(p.size()>=2&&p.front()==L'"'&&p.back()==L'"')p=p.substr(1,p.size()-2);openPath(p);}else{wchar_t f[MAX_PATH]{};OPENFILENAMEW of{sizeof(of)};of.hwndOwner=g_hwnd;of.lpstrFile=f;of.nMaxFile=MAX_PATH;of.lpstrFilter=L"Images and archives\0*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.tif;*.tiff;*.webp;*.avif;*.zip;*.cbz\0All files\0*.*\0";of.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST;if(GetOpenFileNameW(&of))openPath(f);}MSG msg;while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}CoUninitialize();return (int)msg.wParam;}
