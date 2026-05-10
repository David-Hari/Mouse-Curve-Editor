/*
 * Simple program to view and edit the mouse acceleration curve in Windows.
 * To compile:
 *  > rc app.rc
 *  > cl "mouse curve.c" app.res /link user32.lib shell32.lib advapi32.lib /SUBSYSTEM:WINDOWS
 */

#include <windows.h>
#include <CommCtrl.h>
#include <stdio.h>
#include <stdint.h>
#include <cmath>
#include "resource.h"

#pragma comment(linker, \
  "\"/manifestdependency:type='Win32' "\
  "name='Microsoft.Windows.Common-Controls' "\
  "version='6.0.0.0' "\
  "processorArchitecture='*' "\
  "publicKeyToken='6595b64144ccf1df' "\
  "language='*'\"")



template<typename T>
struct Point2 {
	T x;
	T y;
};

using Point2i = Point2<int>;
using Point2d = Point2<double>;


constexpr auto MAX_LOADSTRING = 100;
CHAR szTitle[MAX_LOADSTRING];
HWND graph;
static Point2d points[5];
static int dragIndex = -1;


static double fixedToDouble(uint32_t value) {
	return value / 65536.0;
}

static uint32_t doubleToFixed(double value) {
	return (uint32_t)std::round(value * 65536.0);
}



static void showError(HWND hWnd, const CHAR* action, LSTATUS status) {
	LPTSTR systemMessage = nullptr;

	FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER |
		FORMAT_MESSAGE_FROM_SYSTEM |
		FORMAT_MESSAGE_IGNORE_INSERTS,
		nullptr, status, 0,
		systemMessage, 0,
		nullptr
	);

	CHAR buffer[1024];
	sprintf_s(buffer, "%s failed.\n\nError 0x%08lX\n%s", action, status, systemMessage);

	if (systemMessage) {
		LocalFree(systemMessage);
	}

	MessageBox(hWnd, buffer, "Error", MB_ICONERROR);
}


static LSTATUS loadCurveFromRegistry() {
	HKEY key;
	LSTATUS status;

	if ((status = RegOpenKeyEx(HKEY_CURRENT_USER, "Control Panel\\Mouse", 0, KEY_READ, &key)) != ERROR_SUCCESS) {
		return status;
	}

	BYTE xData[40];
	BYTE yData[40];
	DWORD type;
	DWORD size = sizeof(xData);
	if ((status = RegQueryValueEx(key, "SmoothMouseXCurve", nullptr, &type, xData, &size)) != ERROR_SUCCESS) {
		RegCloseKey(key);
		return status;
	}
	size = sizeof(yData);
	if ((status = RegQueryValueEx(key, "SmoothMouseYCurve", nullptr, &type, yData, &size)) != ERROR_SUCCESS) {
		RegCloseKey(key);
		return status;
	}
	RegCloseKey(key);

	for (int i = 0; i < 5; ++i) {
		uint32_t xv = *(uint32_t*)(xData + i * 8);
		uint32_t yv = *(uint32_t*)(yData + i * 8);

		points[i].x = fixedToDouble(xv);
		points[i].y = fixedToDouble(yv);
	}

	return ERROR_SUCCESS;
}


static LSTATUS saveCurveToRegistry() {
	HKEY key;
	LSTATUS status;

	if ((status = RegOpenKeyEx(HKEY_CURRENT_USER, "Control Panel\\Mouse", 0, KEY_SET_VALUE, &key)) != ERROR_SUCCESS) {
		return status;
	}

	uint32_t xData[5];
	uint32_t yData[5];
	for (int i = 0; i < 5; ++i) {
		xData[i] = doubleToFixed(points[i].x);
		yData[i] = doubleToFixed(points[i].y);
	}

	if ((status = RegSetValueEx(key, "SmoothMouseXCurve", 0, REG_BINARY, (BYTE*)xData, sizeof(xData))) != ERROR_SUCCESS) {
		RegCloseKey(key);
		return status;
	}
	if ((status = RegSetValueEx(key, "SmoothMouseYCurve", 0, REG_BINARY, (BYTE*)yData, sizeof(yData))) != ERROR_SUCCESS) {
		RegCloseKey(key);
		return status;
	}
	RegCloseKey(key);

	// Notify operating system of change so it can take effect immediately
	if (!SystemParametersInfo(SPI_SETMOUSE, 0, NULL, SPIF_SENDCHANGE)) {
		return GetLastError();
	}

	return ERROR_SUCCESS;
}


static Point2i mapGraphToScreen(RECT rc, Point2d input, Point2d max) {
	return {
		(int)((input.x / max.x) * (rc.right - rc.left)),
		(int)(rc.bottom - (input.y / max.y) * (rc.bottom - rc.top))
	};
}


static Point2d mapScreenToGraph(RECT rc, Point2i input, Point2d max) {
	Point2d p;
	p.x = (input.x / (rc.right - rc.left)) * max.x;
	p.y = ((rc.bottom - input.y) / (rc.bottom - rc.top)) * max.y;
	if (p.x < 0) { p.x = 0; }
	if (p.y < 0) { p.y = 0; }
	return p;
}


static void drawGraph(HDC hdc, RECT rc) {
	FillRect(hdc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
	SetBkMode(hdc, TRANSPARENT);

	Point2d max = points[4];
	for (int i = 0; i < 4; ++i) {
		Point2i p1 = mapGraphToScreen(rc, points[i], max);
		Point2i p2 = mapGraphToScreen(rc, points[i + 1], max);

		MoveToEx(hdc, p1.x, p1.y, nullptr);
		LineTo(hdc, p2.x, p2.y);
	}

	for (int i = 0; i < 5; ++i) {
		Point2i p = mapGraphToScreen(rc, points[i], max);
		Ellipse(hdc, p.x - 5, p.y - 5, p.x + 5, p.y + 5);
	}
}


LRESULT CALLBACK WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	LSTATUS status;
	switch (uMsg) {
		case WM_INITDIALOG: {
			HICON hIcon = LoadIcon((HINSTANCE)GetWindowLongPtr(hWnd, GWLP_HINSTANCE), MAKEINTRESOURCE(IDI_MAIN_ICON));
			SendMessage(hWnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
			SendMessage(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
			graph = GetDlgItem(hWnd, IDC_GRAPH);
			if ((status = loadCurveFromRegistry()) != ERROR_SUCCESS) {
				showError(hWnd, "Load from registry", status);
			}
			break;
		}
		case WM_COMMAND: {
			switch (LOWORD(wParam)) {
				case IDC_LOAD:
					if ((status = loadCurveFromRegistry()) == ERROR_SUCCESS) {
						InvalidateRect(GetDlgItem(hWnd, IDC_GRAPH), nullptr, TRUE);
					}
					else {
						showError(hWnd, "Load from registry", status);
					}
					break;

				case IDC_SAVE:
					if ((status = saveCurveToRegistry()) != ERROR_SUCCESS) {
						showError(hWnd, "Save to registry", status);
					}
					break;

				case IDCANCEL:
					EndDialog(hWnd, 0);
					break;
			}
			break;
		}
		case WM_LBUTTONDOWN: {
			RECT rc;
			GetWindowRect(graph, &rc);

			MapWindowPoints(nullptr, hWnd, (LPPOINT)&rc, 2);

			/*int mx = GET_X_LPARAM(lParam);
			int my = GET_Y_LPARAM(lParam);

			for (int i = 0; i < 5; ++i) {
				int px, py;

				graphToScreen(
					rc,
					g_points[i].x,
					g_points[i].y,
					px,
					py
				);

				int dx = mx - px;
				int dy = my - py;

				if ((dx * dx + dy * dy) < 100) {
					g_dragIndex = i;
					SetCapture(hwnd);
					break;
				}
			}*/
			break;
		}
		case WM_MOUSEMOVE: {
			/*if (dragIndex >= 0) {
				RECT rc;
				GetWindowRect(graph, &rc);

				MapWindowPoints(
					nullptr,
					hwnd,
					(LPPOINT)&rc,
					2
				);

				double x, y;

				screenToGraph(
					rc,
					GET_X_LPARAM(lParam),
					GET_Y_LPARAM(lParam),
					x,
					y
				);

				g_points[g_dragIndex].x = x;
				g_points[g_dragIndex].y = y;

				InvalidateRect(graph, nullptr, TRUE);
			}*/
			break;
		}
		case WM_LBUTTONUP: {
			dragIndex = -1;
			ReleaseCapture();
			return TRUE;
		}
		case WM_DRAWITEM: {
			DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lParam;

			if (dis->CtlID == IDC_GRAPH) {
				drawGraph(dis->hDC, dis->rcItem);
				return TRUE;
			}
			break;
		}
		default:
			return DefWindowProc(hWnd, uMsg, wParam, lParam);
	}
	return 0;
}


int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
	INITCOMMONCONTROLSEX icc = {};
	icc.dwSize = sizeof(icc);
	icc.dwICC = ICC_WIN95_CLASSES;
	InitCommonControlsEx(&icc);

	LoadString(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
	
	return (int)DialogBox(hInstance, MAKEINTRESOURCE(ID_MAIN_WINDOW), nullptr, WindowProc);
}