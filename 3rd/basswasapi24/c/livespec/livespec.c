/*
	WASAPI "live" spectrum analyser example
	Copyright (c) 2002-2017 Un4seen Developments Ltd.
*/

#include <windows.h>
#include <stdio.h>
#include <math.h>
#include <malloc.h>
#include "basswasapi.h"
#include "bass.h"

#define SPECWIDTH 368	// display width
#define SPECHEIGHT 127	// height (changing requires palette adjustments too)

HWND win;
DWORD timer;

BASS_WASAPI_INFO info;

HDC specdc;
HBITMAP specbmp;
BYTE *specbuf;

int specmode, specpos; // spectrum mode (and marker pos for 2nd mode)

// display error messages
void Error(const char *es)
{
	char mes[200];
	sprintf(mes, "%s\n(error code: %d)", es, BASS_ErrorGetCode());
	MessageBox(win, mes, 0, 0);
}

// update the spectrum display - the interesting bit :)
void CALLBACK UpdateSpectrum(UINT uTimerID, UINT uMsg, DWORD dwUser, DWORD dw1, DWORD dw2)
{
	static DWORD quietcount;
	HDC dc;
	int x, y, y1;

	if (specmode == 3) { // waveform
		float *buf = (float*)alloca(SPECWIDTH * info.chans * sizeof(float)), *p; // allocate buffer for data
		BASS_WASAPI_GetData(buf, SPECWIDTH * info.chans * sizeof(float)); // get the sample data
		memset(specbuf, 0, SPECWIDTH * SPECHEIGHT);
		for (p = buf, x = 0; x < SPECWIDTH; x++) {
			int a;
			float s = 0;
			for (a = 0; a < info.chans; a++) s += *p++; // sum all channels
			s /= info.chans;
			y1 = (1 - s) * SPECHEIGHT / 2; // invert and scale to fit display
			if (y1 < 0) y1 = 0;
			else if (y1 >= SPECHEIGHT) y1 = SPECHEIGHT - 1;
			if (!x) y = y1;
			do { // draw line from previous sample...
				if (y < y1) y++;
				else if (y > y1) y--;
				specbuf[y * SPECWIDTH + x] = abs(y - SPECHEIGHT / 2) * 2 + 1;
			} while (y != y1);
		}
	} else {
		float fft[1024];
		BASS_WASAPI_GetData(fft, BASS_DATA_FFT2048); // get the FFT data

		if (!specmode) { // "normal" FFT
			memset(specbuf, 0, SPECWIDTH * SPECHEIGHT);
			for (x = 0; x < SPECWIDTH / 2; x++) {
#if 1
				y = sqrt(fft[x + 1]) * 3 * SPECHEIGHT - 4; // scale it (sqrt to make low values more visible)
#else
				y = fft[x + 1] * 10 * SPECHEIGHT; // scale it (linearly)
#endif
				if (y > SPECHEIGHT) y = SPECHEIGHT; // cap it
				if (x && (y1 = (y + y1) / 2)) // interpolate from previous to make the display smoother
					while (--y1 >= 0) specbuf[y1 * SPECWIDTH + x * 2 - 1] = y1 + 1;
				y1 = y;
				while (--y >= 0) specbuf[y * SPECWIDTH + x * 2] = y + 1; // draw level
			}
		} else if (specmode == 1) { // logarithmic, acumulate & average bins
			int b0 = 0;
			memset(specbuf, 0, SPECWIDTH * SPECHEIGHT);
#define BANDS 28
			for (x = 0; x < BANDS; x++) {
				float peak = 0;
				int b1 = pow(2, x * 10.0 / (BANDS - 1));
				if (b1 > 1023) b1 = 1023;
				if (b1 <= b0) b1 = b0 + 1; // make sure it uses at least 1 FFT bin
				for (; b0 < b1; b0++)
					if (peak < fft[1 + b0]) peak = fft[1 + b0];
				y = sqrt(peak) * 3 * SPECHEIGHT - 4; // scale it (sqrt to make low values more visible)
				if (y > SPECHEIGHT) y = SPECHEIGHT; // cap it
				while (--y >= 0)
					memset(specbuf + y * SPECWIDTH + x * (SPECWIDTH / BANDS), y + 1, 0.9 * (SPECWIDTH / BANDS)); // draw bar
			}
		} else { // "3D"
			for (x = 0; x < SPECHEIGHT; x++) {
				y = sqrt(fft[x + 1]) * 3 * 127; // scale it (sqrt to make low values more visible)
				if (y > 127) y = 127; // cap it
				specbuf[x * SPECWIDTH + specpos] = 128 + y; // plot it
			}
			// move marker onto next position
			specpos = (specpos + 1) % SPECWIDTH;
			for (x = 0; x < SPECHEIGHT; x++) specbuf[x * SPECWIDTH + specpos] = 255;
		}
	}

	// update the display
	dc = GetDC(win);
	BitBlt(dc, 0, 0, SPECWIDTH, SPECHEIGHT, specdc, 0, 0, SRCCOPY);
	if (LOWORD(BASS_WASAPI_GetLevel()) < 500) { // check if it's quiet
		quietcount++;
		if (quietcount > 40 && (quietcount & 16)) { // it's been quiet for over a second
			RECT r = { 0,0,SPECWIDTH,SPECHEIGHT };
			SetTextColor(dc, 0xffffff);
			SetBkMode(dc, TRANSPARENT);
			DrawText(dc, "make some noise!", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		}
	} else
		quietcount = 0; // not quiet
	ReleaseDC(win, dc);
}

// WASAPI callback - not doing anything with the data
DWORD CALLBACK DuffRecording(void *buffer, DWORD length, void *user)
{
	return TRUE; // continue recording
}

// window procedure
LRESULT CALLBACK SpectrumWindowProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	switch (m) {
		case WM_PAINT:
			if (GetUpdateRect(h, 0, 0)) {
				PAINTSTRUCT p;
				HDC dc;
				if (!(dc = BeginPaint(h, &p))) return 0;
				BitBlt(dc, 0, 0, SPECWIDTH, SPECHEIGHT, specdc, 0, 0, SRCCOPY);
				EndPaint(h, &p);
			}
			return 0;

		case WM_LBUTTONUP:
			specmode = (specmode + 1) % 4; // swap spectrum mode
			memset(specbuf, 0, SPECWIDTH * SPECHEIGHT);	// clear display
			return 0;

		case WM_CREATE:
			win = h;
			// initialize "no sound" BASS device
			if (!BASS_Init(0, 44100, 0, h, NULL)) {
				Error("Can't initialize BASS");
				return -1;
			}
			// initialize default loopback input device (or default input device if no loopback)
			if (!BASS_WASAPI_Init(-3, 0, 0, BASS_WASAPI_BUFFER, 1, 0.1, &DuffRecording, NULL)
				&& !BASS_WASAPI_Init(-2, 0, 0, BASS_WASAPI_BUFFER, 1, 0.1, &DuffRecording, NULL)) {
				Error("Can't initialize WASAPI device");
				return -1;
			}
			BASS_WASAPI_GetInfo(&info);
			BASS_WASAPI_Start();
			{ // create bitmap to draw spectrum in (8 bit for easy updating)
				BYTE data[2000] = { 0 };
				BITMAPINFOHEADER *bh = (BITMAPINFOHEADER*)data;
				RGBQUAD *pal = (RGBQUAD*)(data + sizeof(*bh));
				int a;
				bh->biSize = sizeof(*bh);
				bh->biWidth = SPECWIDTH;
				bh->biHeight = SPECHEIGHT; // upside down (line 0=bottom)
				bh->biPlanes = 1;
				bh->biBitCount = 8;
				bh->biClrUsed = bh->biClrImportant = 256;
				// setup palette
				for (a = 1; a < 128; a++) {
					pal[a].rgbGreen = 256 - 2 * a;
					pal[a].rgbRed = 2 * a;
				}
				for (a = 0; a < 32; a++) {
					pal[128 + a].rgbBlue = 8 * a;
					pal[128 + 32 + a].rgbBlue = 255;
					pal[128 + 32 + a].rgbRed = 8 * a;
					pal[128 + 64 + a].rgbRed = 255;
					pal[128 + 64 + a].rgbBlue = 8 * (31 - a);
					pal[128 + 64 + a].rgbGreen = 8 * a;
					pal[128 + 96 + a].rgbRed = 255;
					pal[128 + 96 + a].rgbGreen = 255;
					pal[128 + 96 + a].rgbBlue = 8 * a;
				}
				// create the bitmap
				specbmp = CreateDIBSection(0, (BITMAPINFO*)bh, DIB_RGB_COLORS, (void**)& specbuf, NULL, 0);
				specdc = CreateCompatibleDC(0);
				SelectObject(specdc, specbmp);
			}
			// setup update timer (40hz)
			timer = timeSetEvent(25, 25, (LPTIMECALLBACK)& UpdateSpectrum, 0, TIME_PERIODIC);
			break;

		case WM_DESTROY:
			if (timer) timeKillEvent(timer);
			BASS_WASAPI_Free();
			BASS_Free();
			if (specdc) DeleteDC(specdc);
			if (specbmp) DeleteObject(specbmp);
			PostQuitMessage(0);
			break;
	}
	return DefWindowProc(h, m, w, l);
}

int PASCAL WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	WNDCLASS wc = { 0 };
	MSG msg;

	// check the correct BASS was loaded
	if (HIWORD(BASS_GetVersion()) != BASSVERSION) {
		MessageBox(0, "An incorrect version of BASS.DLL was loaded", 0, MB_ICONERROR);
		return 0;
	}

	// register window class and create the window
	wc.lpfnWndProc = SpectrumWindowProc;
	wc.hInstance = hInstance;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.lpszClassName = "BASS-Spectrum";
	if (!RegisterClass(&wc) || !CreateWindow("BASS-Spectrum",
		"BASSWASAPI \"live\" spectrum (click to toggle mode)",
		WS_POPUPWINDOW | WS_CAPTION | WS_VISIBLE, 200, 200,
		SPECWIDTH + 2 * GetSystemMetrics(SM_CXDLGFRAME),
		SPECHEIGHT + GetSystemMetrics(SM_CYCAPTION) + 2 * GetSystemMetrics(SM_CYDLGFRAME),
		NULL, NULL, hInstance, NULL)) {
		Error("Can't create window");
		return 0;
	}
	ShowWindow(win, SW_SHOWNORMAL);

	while (GetMessage(&msg, NULL, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	return 0;
}
