/*
 * Simple program to view and edit the mouse acceleration curve in Windows.
 * To compile:
 *  > rc app.rc
 *  > cl "mouse curve.c" app.res /link user32.lib shell32.lib advapi32.lib /SUBSYSTEM:WINDOWS
 */

#include <windows.h>
#include <windowsx.h>
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
constexpr auto NUM_POINTS = 5;
static Point2d points[NUM_POINTS];
static HWND mainWindow;
static HWND graph;
static bool hasGraphRect = false;
static int graphXPad, graphYPad, textXPad, textYPad;
static int dragIndex = -1;
static Point2i mouseOffset = { 0, 0 };
constexpr auto CIRCLE_SIZE = 4;  // Pixels at 100% DPI scale
static int circleSize = CIRCLE_SIZE;
static HPEN gridLinePen = CreatePen(PS_SOLID, 1, RGB(220, 220, 220));
static HBRUSH circleBrush = CreateSolidBrush(RGB(100, 100, 100));


static double fixedToDouble(uint32_t value) {
	return value / 65536.0;
}

static uint32_t doubleToFixed(double value) {
	return (uint32_t)std::round(value * 65536.0);
}

static LSTATUS saveCurveToRegistry(HWND hwnd);
static LSTATUS saveCurveToRegistry(HWND hwnd);
static Point2i mapGraphToScreen(RECT rc, Point2d input);
static Point2d mapScreenToGraph(RECT rc, Point2i input);
static void updateEditBox(int i);
static bool updatePointFromEdit(int index);
static SIZE getGraphTextExtent(HDC hdc);
static void drawGraph(HWND hwnd, HDC hdc, RECT rc);
LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK graphSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR);



static void showError(HWND hwnd, const CHAR* action, LSTATUS status) {
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

	MessageBox(hwnd, buffer, "Error", MB_ICONERROR);
}


static LSTATUS loadCurveFromRegistry(HWND hwnd) {
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

	for (int i = 0; i < NUM_POINTS; ++i) {
		uint32_t xValue = 0, yValue = 0;
		memcpy(&xValue, xData + i * 8, sizeof(xValue));
		memcpy(&yValue, yData + i * 8, sizeof(yValue));

		points[i].x = fixedToDouble(xValue);
		points[i].y = fixedToDouble(yValue);

		updateEditBox(i);
	}

	return ERROR_SUCCESS;
}


static LSTATUS saveCurveToRegistry(HWND) {
	HKEY key;
	LSTATUS status;

	if ((status = RegOpenKeyEx(HKEY_CURRENT_USER, "Control Panel\\Mouse", 0, KEY_SET_VALUE, &key)) != ERROR_SUCCESS) {
		return status;
	}

	BYTE xData[40] = { 0 };
	BYTE yData[40] = { 0 };
	for (int i = 0; i < NUM_POINTS; ++i) {
		uint32_t xValue = doubleToFixed(points[i].x);
		uint32_t yValue = doubleToFixed(points[i].y);

		memcpy(xData + i * 8, &xValue, sizeof(xValue));
		memcpy(yData + i * 8, &yValue, sizeof(yValue));
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

	// Notify operating system of change so the curve values can take effect immediately
	SendMessageTimeout(HWND_BROADCAST, WM_SETTINGCHANGE, 0, 0, SMTO_NORMAL, 1000, nullptr);

	return ERROR_SUCCESS;
}


static Point2i mapGraphToScreen(RECT rc, Point2d input) {
	Point2d max = points[NUM_POINTS - 1];
	return {
		rc.left + (int)((input.x / max.x) * (rc.right - rc.left)),
		rc.bottom - (int)((input.y / max.y) * (rc.bottom - rc.top))
	};
}


static Point2d mapScreenToGraph(RECT rc, Point2i input) {
	Point2d max = points[NUM_POINTS - 1];
	Point2d p = {
		((input.x - rc.left) / static_cast<double>(rc.right - rc.left)) * max.x,
		((rc.bottom - input.y) / static_cast<double>(rc.bottom - rc.top)) * max.y
	};
	if (p.x < 0) { p.x = 0; }
	if (p.y < 0) { p.y = 0; }
	return p;
}


static void updateEditBox(int index) {
	TCHAR text[128];
	sprintf_s(text, "%.3g, %.8g", points[index].x, points[index].y);
	SetDlgItemText(mainWindow, IDC_EDIT1 + index, text);
}


static bool updatePointFromEdit(int index) {
	TCHAR text[128];

	GetDlgItemText(mainWindow, IDC_EDIT1 + index, text, _countof(text));

	double x, y;
	if (sscanf_s(text, "%lf, %lf", &x, &y) != 2) {
		return false;
	}

	points[index].x = x;
	points[index].y = y;

	return true;
}


static SIZE getGraphTextExtent(HDC hdc) {
	SIZE textSize = { 0 };
	GetTextExtentPoint32(hdc, "000.0", 5, &textSize);
	return textSize;
}

/* Returns the region to draw the graph in */
static RECT getGraphRect(RECT rc) {
	return {
		rc.left + graphXPad + textXPad,
		rc.top + graphYPad,
		rc.right - graphXPad,
		rc.bottom - graphYPad - textYPad
	};
}


static void drawGraph(HWND hwnd, HDC hdc, RECT rc) {
	HFONT font = (HFONT)SendMessage(hwnd, WM_GETFONT, 0, 0);
	HFONT oldFont = (HFONT)SelectObject(hdc, font);
	RECT graphRect = getGraphRect(rc);

	// Draw the axis lines
	MoveToEx(hdc, graphRect.left, graphRect.bottom, nullptr);
	LineTo(hdc, graphRect.right, graphRect.bottom);
	MoveToEx(hdc, graphRect.left, graphRect.top, nullptr);
	LineTo(hdc, graphRect.left, graphRect.bottom);

	SIZE textSize = getGraphTextExtent(hdc);
	const int tickCount = 10;
	int graphWidth = graphRect.right - graphRect.left;
	int graphHeight = graphRect.bottom - graphRect.top;

	// Draw tick values
	Point2d max = points[NUM_POINTS - 1];
	TCHAR text[64] = { 0 };
	for (int i = 0; i <= tickCount; ++i) {
		double value = (max.x * i) / tickCount;
		int x = graphRect.left + (int)((double)i / tickCount * graphWidth);
		sprintf_s(text, "%.1f", value);
		TextOut(hdc, x - textSize.cx / 2, graphRect.bottom + textYPad, text, lstrlen(text));
	}
	for (int i = 0; i <= tickCount; ++i) {
		double value = (max.y * i) / tickCount;
		int y = graphRect.bottom - (int)((double)i / tickCount * graphHeight);
		sprintf_s(text, "%.1f", value);
		TextOut(hdc, textXPad, y - textSize.cy / 2, text, lstrlen(text));
	}

	// Draw grid lines
	HPEN oldPen = (HPEN)SelectObject(hdc, gridLinePen);
	for (int i = 1; i <= tickCount; ++i) {
		int x = graphRect.left + (int)((double)i / tickCount * graphWidth);
		MoveToEx(hdc, x, graphRect.bottom - 1, nullptr);
		LineTo(hdc, x, graphRect.top);
	}
	for (int i = 1; i <= tickCount; ++i) {
		int y = graphRect.bottom - (int)((double)i / tickCount * graphHeight);
		MoveToEx(hdc, graphRect.left + 1, y, nullptr);
		LineTo(hdc, graphRect.right, y);
	}
	SelectObject(hdc, oldPen);

	// Draw line segments
	for (int i = 0; i < NUM_POINTS - 1; ++i) {
		Point2i p1 = mapGraphToScreen(graphRect, points[i]);
		Point2i p2 = mapGraphToScreen(graphRect, points[i + 1]);

		MoveToEx(hdc, p1.x, p1.y, nullptr);
		LineTo(hdc, p2.x, p2.y);
	}

	// Draw circles on each point
	HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, circleBrush);
	for (int i = 0; i < NUM_POINTS; ++i) {
		Point2i p = mapGraphToScreen(graphRect, points[i]);
		Ellipse(hdc, p.x - circleSize, p.y - circleSize, p.x + circleSize, p.y + circleSize);
	}
	SelectObject(hdc, oldBrush);
	SelectObject(hdc, oldFont);
}


LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	LSTATUS status;
	switch (msg) {
		case WM_INITDIALOG: {
			HICON hIcon = LoadIcon((HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE), MAKEINTRESOURCE(IDI_MAIN_ICON));
			SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
			SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
			mainWindow = hwnd;
			graph = GetDlgItem(hwnd, IDC_GRAPH);
			SetWindowSubclass(graph, graphSubclassProc, 0, 0);
			circleSize = GetDpiForWindow(hwnd) * CIRCLE_SIZE / 96;
			if ((status = loadCurveFromRegistry(hwnd)) != ERROR_SUCCESS) {
				showError(hwnd, "Load from registry", status);
			}
			break;
		}
		case WM_COMMAND: {
			int id = LOWORD(wParam);
			int code = HIWORD(wParam);

			switch (id) {
				case IDC_LOAD:
					if ((status = loadCurveFromRegistry(hwnd)) == ERROR_SUCCESS) {
						InvalidateRect(GetDlgItem(hwnd, IDC_GRAPH), nullptr, TRUE);
					}
					else {
						showError(hwnd, "Load from registry", status);
					}
					return TRUE;
				case IDC_SAVE:
					if ((status = saveCurveToRegistry(hwnd)) != ERROR_SUCCESS) {
						showError(hwnd, "Save to registry", status);
					}
					return TRUE;
				case IDCANCEL:
					EndDialog(hwnd, 0);
					return TRUE;
			}

			if (id >= IDC_EDIT1 && id <= IDC_EDIT5) {
				if (code == EN_KILLFOCUS || code == EN_UPDATE) {
					if (updatePointFromEdit(id - IDC_EDIT1)) {
						InvalidateRect(graph, nullptr, TRUE);
					}
					else {
						updateEditBox(id - IDC_EDIT1);
					}
				}
				return TRUE;
			}
			break;
		}
		default:
			return DefWindowProc(hwnd, msg, wParam, lParam);
	}
	return 0;
}


LRESULT CALLBACK graphSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
	switch (msg) {
		case WM_LBUTTONDOWN: {
			RECT rc;
			GetClientRect(hwnd, &rc);
			Point2i mousePos = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
			RECT graphRect = getGraphRect(rc);

			for (int i = 0; i < NUM_POINTS; ++i) {
				Point2i p = mapGraphToScreen(graphRect, points[i]);

				int dx = mousePos.x - p.x;
				int dy = mousePos.y - p.y;

				// Check if within circle (plus a bit of padding)
				double dist = circleSize * 1.5;
				if ((dx * dx + dy * dy) < (dist * dist)) {
					dragIndex = i;
					mouseOffset = { dx, dy };
					break;
				}
			}
			return 0;
		}
		case WM_MOUSEMOVE: {
			if (dragIndex >= 0) {
				RECT rc;
				GetClientRect(hwnd, &rc);
				Point2i pos = { GET_X_LPARAM(lParam) - mouseOffset.x, GET_Y_LPARAM(lParam) - mouseOffset.y };

				Point2d p = mapScreenToGraph(getGraphRect(rc), pos);
				points[dragIndex].x = p.x;
				points[dragIndex].y = p.y;

				updateEditBox(dragIndex);
				InvalidateRect(hwnd, nullptr, TRUE);
			}
			break;
		}
		case WM_LBUTTONUP: {
			dragIndex = -1;
			mouseOffset = { 0, 0 };
			return TRUE;
		}
		case WM_ERASEBKGND: {
			HDC hdc = (HDC)wParam;
			RECT rc;
			GetClientRect(hwnd, &rc);
			FillRect(hdc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
			return 1;
		}
		case WM_PAINT: {
			PAINTSTRUCT ps;
			RECT rc;
			HDC hdc = BeginPaint(hwnd, &ps);
			GetClientRect(hwnd, &rc);
			if (!hasGraphRect) {
				SIZE textSize = getGraphTextExtent(hdc);
				graphXPad = (int)(textSize.cx * 1.5);
				graphYPad = (int)(textSize.cy * 1.5);
				textXPad = (int)(textSize.cx * 0.25);
				textYPad = (int)(textSize.cy * 0.25);
				hasGraphRect = true;
			}
			drawGraph(hwnd, hdc, rc);
			EndPaint(hwnd, &ps);
			return 0;
		}
	}
	return DefSubclassProc(hwnd, msg, wParam, lParam);
}


int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
	INITCOMMONCONTROLSEX icc = {};
	icc.dwSize = sizeof(icc);
	icc.dwICC = ICC_WIN95_CLASSES;
	InitCommonControlsEx(&icc);

	LoadString(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
	
	return (int)DialogBox(hInstance, MAKEINTRESOURCE(ID_MAIN_WINDOW), nullptr, windowProc);
}