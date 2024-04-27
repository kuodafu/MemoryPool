#if defined _M_IX86
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='x86' publicKeyToken='6595b64144ccf1df' language='*'\"")
#elif defined _M_IA64
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='ia64' publicKeyToken='6595b64144ccf1df' language='*'\"")
#elif defined _M_X64
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'\"")
#else
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif


#include <SDKDDKVer.h>
#define WIN32_LEAN_AND_MEAN             // 从 Windows 头文件中排除极少使用的内容
// Windows 头文件
#include <windows.h>
#include <windowsx.h>
#include <CommCtrl.h>
// C 运行时头文件
#include <stdlib.h>
#include <malloc.h>
#include <memory.h>
#include <tchar.h>
#include <future>
#include "../CMemoryObjectPool.h"



#define ID_INIT_POOL            1000    // 绘画内存池分配器
#define ID_ALLOCATOR            1001    // 分配一个成员
#define ID_ALLOCATOR2           1002    // 分配两个成员
#define ID_FREE                 1003    // 释放一个成员

class CAllocator : public std::allocator<BYTE>
{
    using _Ty = BYTE;
public:

    void deallocate(_Ty* const _Ptr, const size_t _Count) {
        _STL_ASSERT(_Ptr != nullptr || _Count == 0, "null pointer cannot point to a block of non-zero size");
        if (_Ptr)
            VirtualFree(_Ptr, 0, MEM_RELEASE);

    }

    __declspec(allocator) _Ty* allocate(_CRT_GUARDOVERFLOW const size_t _Count) {
        static_assert(sizeof(value_type) > 0, "value_type must be complete before calling allocate.");

        LPBYTE pStart = (LPBYTE)0x50000;
        for (int i = 0; i < 100; i++)
        {
            // 演示用, 就不管对齐的问题了
            if (void* const block = VirtualAlloc(pStart, _Count, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))
            {
                return reinterpret_cast<_Ty*>(block);
            }
            pStart += 0x10000;
        }
        throw std::bad_alloc();
        return 0;
    }

};

typedef int value_type;
#if CMEMORYPOOL_ISDEBUG
static kuodafu::CMemoryObjectPool pool;
static kuodafu::CMemoryPoolView pool_view;
#else
static kuodafu::CMemoryObjectPool<value_type, CAllocator> pool;
static kuodafu::CMemoryPoolView<value_type, CAllocator> pool_view;
#endif

const int m_headSize = sizeof(kuodafu::MEMORY_HEAD);
const int m_itemSize = sizeof(int);
const int m_count = 10;
static HFONT m_hFont;
struct ITEM_VALUE
{
    value_type* pAddr;  // 记录内存池分配到的成员地址
    value_type* pArr;   // 如果这个成员是数组, 那么这个记录数组首地址
    int count;          // 如果这个成员是数组, 那么这个记录数组的成员个数
    int start;          // 起始索引, 方便释放用
};

static int m_id = ID_INIT_POOL;
static ITEM_VALUE m_data[m_count];        // 记录当前分配的地址
static RECT m_rcBlock[m_count];     // 10个内存块的矩形尺寸, 右键菜单需要使用
static int m_index_down = -1;       // 右键弹出菜单时的索引
static int m_index = -1;            // 左键点击的索引
// 此代码模块中包含的函数的前向声明:
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
void DrawAllocator(HWND hWnd, HDC hdc, const RECT& rc);



int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    pool.init(m_count);
    pool_view.init(&pool);

    // 执行应用程序初始化:
    if (!InitInstance (hInstance, nCmdShow))
    {
        return FALSE;
    }

    MSG msg;

    // 主消息循环:
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return (int) msg.wParam;
}


//
//   函数: InitInstance(HINSTANCE, int)
//
//   目标: 保存实例句柄并创建主窗口
//
//   注释:
//
//        在此函数中，我们在全局变量中保存实例句柄并
//        创建和显示主程序窗口。
//
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
    WNDCLASSEXW wcex;

    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style          = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc    = WndProc;
    wcex.cbClsExtra     = 0;
    wcex.cbWndExtra     = 0;
    wcex.hInstance      = hInstance;
    wcex.hIcon          = LoadIconW(nullptr, IDI_APPLICATION);
    wcex.hCursor        = LoadCursorW(nullptr, IDC_ARROW);
    wcex.hbrBackground  = (HBRUSH)(COLOR_WINDOW+1);
    wcex.lpszMenuName   = 0;
    wcex.lpszClassName  = L"扩大福内存池 - 视图";
    wcex.hIconSm        = wcex.hIcon;

    RegisterClassExW(&wcex);
    HWND hWnd = CreateWindowW(wcex.lpszClassName, L"扩大福内存池 - 可视化视图", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
      CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, nullptr);

    if (!hWnd)
    {
        return FALSE;
    }

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    return TRUE;
}

//
//  函数: WndProc(HWND, UINT, WPARAM, LPARAM)
//
//  目标: 处理主窗口的消息。
//
//  WM_COMMAND  - 处理应用程序菜单
//  WM_PAINT    - 绘制主窗口
//  WM_DESTROY  - 发送退出消息并返回
//
//
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{

    switch (message)
    {
    case WM_CREATE:
    {
        LOGFONTW lf = { 0 };
        SystemParametersInfoW(SPI_GETICONTITLELOGFONT, sizeof(lf), &lf, 0);
        lf.lfHeight = -16;
        lf.lfCharSet = GB2312_CHARSET;
        memcpy(lf.lfFaceName, L"宋体", 6);
        m_hFont = CreateFontIndirectW(&lf);

        auto pfn_create = [hWnd](int x, int y, int cx, int cy, int id, LPCWSTR pszTitle)
        {
            const int style = WS_CLIPSIBLINGS | WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON;
            HWND hChild = CreateWindowExW(0, WC_BUTTONW, pszTitle, style, x, y, cx, cy, hWnd, (HMENU)(ULONG_PTR)id, 0, 0);
            SendMessageW(hChild, WM_SETFONT, (WPARAM)m_hFont, 0);
            return hChild;
        };

        pfn_create(10, 10, 100, 30, ID_INIT_POOL, L"初始化内存池");
        pfn_create(10, 48, 100, 30, ID_ALLOCATOR, L"分配一个成员");
        pfn_create(10, 86, 100, 30, ID_ALLOCATOR2, L"分配两个成员");
        //pfn_create(10, 124, 100, 30, ID_FREE, L"释放一个成员");
    }
    break;
    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);
        switch (id)
        {
        case ID_INIT_POOL:
        {
            pool.clear();
            memset(m_data, 0, sizeof(m_data));


            m_id = id;
            m_index_down = -1;
            InvalidateRect(hWnd, 0, TRUE);
            break;
        }
        case ID_ALLOCATOR:
        case ID_ALLOCATOR2:
        {
            auto pfn_alloc = [hWnd](int count)
            {
                auto pHead = pool_view.GetHead();
                LPBYTE pFirst = 0;
                if (pHead->item == 0)
                {
                    // arr里已经没有内存可以分配了, 从回收的链表里分配
                    kuodafu::PLIST_NODE pNextNode = 0;
                    const int node_count = pool_view.GetNodeCount(pHead, pHead->list, pNextNode);
                    if (node_count < count)
                    {
                        // 链表里也没有了, 这里就不申请了, 实际的内存池就是分开另一块更大的内存块, 然后重复操作
                        MessageBoxW(hWnd, L"内存池已经不够分配了, 需要分配另一块更大的内存块", L"这里就不演示了", 0);
                        return;
                    }

                    pFirst = (LPBYTE)pHead->list;
                }
                else
                {
                    pFirst = (LPBYTE)pHead->item;
                }

                const int index = pool_view.PointerToIndex(pHead, pFirst);
                if (count > 1)
                {
                    PINT pArr = pool.malloc_arr(count);
                    for (int i = index; i < index + count; i++)
                    {
                        m_data[i].pAddr = &pArr[i - index];
                        m_data[i].pArr = pArr;
                        m_data[i].count = count;
                        m_data[i].start = index;
                    }
                }
                else
                {
                    m_data[index].pAddr = pool.malloc();
                    m_data[index].pArr = 0;
                    m_data[index].count = count;
                    m_data[index].start = index;
                }
            };

            pfn_alloc(id == ID_ALLOCATOR2 ? 2 : 1);
            InvalidateRect(hWnd, 0, TRUE);
            break;
        }
        case ID_FREE:
        {
            if(m_index_down != -1)
            {
                ITEM_VALUE& item = m_data[m_index_down];
                auto pHead = pool_view.GetHead();
                const int index = pool_view.PointerToIndex(pHead, item.pAddr);
                if (item.count > 1)
                {
                    const int count = item.count;
                    const int start = item.start;
                    pool.free_arr(item.pArr, item.count);
                    for (int i = start; i < start + count; i++)
                    {
                        memset(&m_data[i], 0, sizeof(m_data[i]));
                    }
                }
                else
                {
                    pool.free(item.pAddr);
                    memset(&item, 0, sizeof(item));
                }

                InvalidateRect(hWnd, 0, TRUE);
            }
            break;
        }
        default:
            return DefWindowProcW(hWnd, message, wParam, lParam);
        }
        break;
    }
    break;
    case WM_RBUTTONDOWN:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        m_index_down = -1;
        for (int i = 0; i < ARRAYSIZE(m_rcBlock); i++)
        {
            if (PtInRect(&m_rcBlock[i], pt))
            {
                // 已经分配的内存块才能释放
                if (m_data[i].pAddr)
                    m_index_down = i;
                break;
            }
        }
        if (m_index_down != -1)
        {
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, ID_FREE, L"释放这个成员");
            ClientToScreen(hWnd, &pt);
            TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, 0);
            DestroyMenu(hMenu);
        }
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }
    case WM_LBUTTONDOWN:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int oldIndex = m_index;
        m_index = -1;
        for (int i = 0; i < ARRAYSIZE(m_rcBlock); i++)
        {
            if (PtInRect(&m_rcBlock[i], pt))
            {
                m_index = i;
                break;
            }
        }
        if (m_index != oldIndex)
        {
            InvalidateRect(hWnd, 0, TRUE);
        }
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        RECT rc;
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        GetClientRect(hWnd, &rc);
        const int cxClient = rc.right - rc.left;
        const int cyClient = rc.bottom - rc.top;

        // 创建设备无关位图
        HDC hdcMem = CreateCompatibleDC(0);
        BITMAPINFO bm = { 0 };
        bm.bmiHeader.biSize = sizeof(bm.bmiHeader);
        bm.bmiHeader.biWidth = cxClient;
        bm.bmiHeader.biHeight = cyClient;
        bm.bmiHeader.biPlanes = 1;
        bm.bmiHeader.biBitCount = 32;
        bm.bmiHeader.biSizeImage = bm.bmiHeader.biWidth * bm.bmiHeader.biHeight * bm.bmiHeader.biBitCount / 8;
        void* pBits = 0;
        HBITMAP hBitmap = CreateDIBSection(hdcMem, &bm, DIB_RGB_COLORS, &pBits, 0, 0);
        if (!hBitmap)
            throw;
        SelectObject(hdcMem, hBitmap);
        SetBkMode(hdcMem, TRANSPARENT);
        SelectObject(hdcMem, m_hFont);

        COLORREF clrBk = RGB(255, 255, 255);
        COLORREF clrText = RGB(0, 0, 0);
        SetBkColor(hdcMem, clrBk);
        SetTextColor(hdcMem, clrText);

        HBRUSH hbrBk = CreateSolidBrush(clrBk);
        HPEN hPen = CreatePen(PS_INSIDEFRAME, 1, RGB(0, 0, 0));
        SelectObject(hdcMem, hbrBk);
        SelectObject(hdcMem, hPen);
        FillRect(hdcMem, &rc, hbrBk);


        switch (m_id)
        {
        case ID_INIT_POOL:
            DrawAllocator(hWnd, hdcMem, rc);
            break;
        default:
            break;
        }


        BitBlt(ps.hdc, 0, 0, cxClient, cyClient, hdcMem, 0, 0, SRCCOPY);
        EndPaint(hWnd, &ps);

        DeleteObject(hPen);
        DeleteObject(hbrBk);
        DeleteObject(hBitmap);
        DeleteDC(hdcMem);
    }
    break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }
    return 0;
}

template<typename _Fn, typename... _Args>
void draw_block(HDC hdc, RECT& rcBlock, LPCWSTR pszText, HBRUSH hbr, _Fn&& _Fx, _Args&&... _Ax)
{
    HGDIOBJ hOldObj = 0;
    if (hbr)
        hOldObj = SelectObject(hdc, hbr);

    Rectangle(hdc, rcBlock.left - 1, rcBlock.top, rcBlock.right, rcBlock.bottom);
    if (pszText && *pszText)
        DrawTextW(hdc, pszText, -1, &rcBlock, DT_SINGLELINE | DT_VCENTER | DT_CENTER);

    _Fx(std::forward<_Args>(_Ax)...);

    OffsetRect(&rcBlock, rcBlock.right - rcBlock.left, 0);
    if (hOldObj)
        SelectObject(hdc, hOldObj);
}

void DrawAllocator(HWND hWnd, HDC hdc, const RECT& rc)
{
    const int cxClient = rc.right - rc.left;
    const int cyClient = rc.bottom - rc.top;
    const int offset_left = 120;
    auto pHead = pool_view.GetHead();
    const LPBYTE pThis = (LPBYTE)pHead;   // 内存块的起始地址

    RECT rcText = { offset_left + 30, 0, cxClient, 45 };
    wchar_t buf_text[100] = { 0 };
    swprintf_s(buf_text, L"每个成员占用%d个字节, 这个内存池有%d个成员, 这个内存块的起始地址是 0x%p", m_itemSize, m_count, pThis);

    DrawTextW(hdc, buf_text, -1, &rcText, DT_SINGLELINE | DT_BOTTOM);

    LPBYTE pItemStart = 0, pItemEnd = 0;
    pool_view.GetItemStartEnd(pHead, pItemStart, pItemEnd);

    int top = 50;
    int left = offset_left + 30;
    int width = 120;
    int height = width;
    RECT rcBlock = { left, top, left + 60, top + height };
    COLORREF clrBlock = RGB(222, 255, 222);     // 待分配颜色
    COLORREF clrAllocated = RGB(255, 222, 222); // 已分配颜色
    COLORREF clrFreeList = RGB(222, 222, 255);  // 回收链表颜色
    HBRUSH hbrBlock = CreateSolidBrush(clrBlock);
    HBRUSH hbrAllocated = CreateSolidBrush(clrAllocated);
    HBRUSH hbrFreeList = CreateSolidBrush(clrFreeList);
    HBRUSH hbr = hbrBlock;

    RECT rcBorder{ left, top, left + m_count * width + 60, top + height };
    Rectangle(hdc, rcBorder.left, rcBorder.top, rcBorder.right, rcBorder.bottom);

    LPBYTE pFreeList = (LPBYTE)pHead->list;
    kuodafu::PLIST_NODE pNextNode = 0;
    const int first_node_count = pool_view.GetNodeCount(pHead, pHead->list, pNextNode);
    const int first_node_index = pHead->list ? pool_view.PointerToIndex(pHead, pHead->list) : -1;
    const int first_arr_index = pHead->item ? pool_view.PointerToIndex(pHead, pHead->item) : 0x7fffffff;

    auto pfn_draw_head = [&rcBlock, hdc, pHead, cxClient]()
    {
        int mid = rcBlock.left + (rcBlock.right - rcBlock.left) / 2;
        MoveToEx(hdc, mid, rcBlock.bottom, 0);
        LineTo(hdc, mid, rcBlock.bottom + 400);
        LineTo(hdc, mid + 50, rcBlock.bottom + 400);
        const int height = 170;
        const int width = 450;

        RECT rc;
        rc.left = mid + 50;
        rc.right = rc.left + width;
        rc.top = rcBlock.bottom + 400 - height / 2;
        rc.bottom = rc.top + height;

        Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);

        OffsetRect(&rc, 10, 10);
        rc.right -= 20;
        rc.bottom -= 20;

        wchar_t szSize[16] = { 0 };
        const int nSize = swprintf_s(szSize, L"%d", pHead->size);
        for (int i = nSize; i < 10; i++)
            szSize[i] = L' ';
        szSize[10] = L'\0';

        LPCWSTR fmt =
            L"内存块结构信息: 结构占用尺寸%d字节\r\n"
            L"this : 0x%p    内存块的起始地址\r\n\r\n"
            L"next : 0x%p    下一个内存块地址\r\n"
            L"size : %s    内存块结构 + %d个成员占用的尺寸\r\n"
            L"list : 0x%p    回收回来的内存\r\n"
            L"item : 0x%p    下一个分配的内存地址"
            ;


        wchar_t szText[1024] = { 0 };
        swprintf_s(szText, fmt, m_headSize, pHead, pHead->next, szSize, m_count, pHead->list, pHead->item);
        DrawTextW(hdc, szText, -1, &rc, DT_LEFT);

        if (pHead->list == 0)
            return;
        
        std::wstring str;
        str.reserve(100);
        str.assign(L"pFree链表每个节点的信息\r\n\r\n");
        auto node = pHead->list;
        int i = 0;
        while (node)
        {
            kuodafu::PLIST_NODE pNextNode = 0;

            const int count = pool_view.GetNodeCount(pHead, node, pNextNode);
            LPBYTE pNodeStart = reinterpret_cast<LPBYTE>(node);
            LPBYTE pNodeEnd = pNodeStart + (m_itemSize * count);

            wchar_t sz[128] = { 0 };
            swprintf_s(sz, L"%d: 0x%p - 0x%p, 占用 %d 个成员, 下一个节点 = 0x%p\r\n", i++, pNodeStart, pNodeEnd, count, pNextNode);
            str.append(sz);

            node = pNextNode;
        }

        OffsetRect(&rc, width, 0);
        rc.bottom += 80;
        rc.right = cxClient;
        DrawTextW(hdc, str.c_str(), (int)str.size(), &rc, DT_LEFT);

    };
    draw_block(hdc, rcBlock, L"内存块", 0, pfn_draw_head);

    auto pfn_draw_remark = [&rcBlock, hdc, cxClient](int width, int height, int height2, LPCWSTR pszText)
    {
        const int bottom = rcBlock.bottom + height / 2 + height2;
        int mid = rcBlock.left + (rcBlock.right - rcBlock.left) / 2;

        MoveToEx(hdc, mid, rcBlock.bottom, 0);
        LineTo(hdc, mid, bottom);

        RECT rc;
        rc.left = mid + 50;
        rc.right = rc.left + width;
        rc.top = bottom - height / 2;
        rc.bottom = rc.top + height;
        if (rc.right >= cxClient)
        {
            // 往右的话, 已经超出了客户区, 矩形向左
            rc.right = mid - 50;
            rc.left = rc.right - width;
            LineTo(hdc, mid - 50, bottom);
        }
        else
        {
            LineTo(hdc, mid + 50, bottom);
        }
        Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);

        OffsetRect(&rc, 10, 10);
        rc.right -= 20;
        rc.bottom -= 20;

        DrawTextW(hdc, pszText, -1, &rc, DT_LEFT);
    };
    auto pfn_draw_freelist = [&](int i)
    {
        LPBYTE pItemStart = 0, pItemEnd = 0;
        pool_view.GetItemStartEnd(pHead, pItemStart, pItemEnd);
        LPBYTE pItem = pItemStart + i * m_itemSize;

        kuodafu::PLIST_NODE pFree = pHead->list;
        kuodafu::PLIST_NODE pNext = 0;
        kuodafu::PLIST_NODE node = pFree;
        int node_index = -1;
        int node_count = 0;
        while (node)
        {
            int count = pool_view.GetNodeCount(pHead, node, pNext);
            LPBYTE pNodeStart = reinterpret_cast<LPBYTE>(node);
            LPBYTE pNodeEnd = pNodeStart + (sizeof(value_type) * count);
            if(pItem >= pNodeStart && pItem < pNodeEnd)
            {
                node_index = pool_view.PointerToIndex(pHead, node);
                node_count = count;
                break;
            }
            node = pNext;
        }
        const int height = 105;
        const int width = 260;
        if (i > node_index && i < node_index + node_count)
        {
            RECT old = rcBlock;
            old.left -= 1;
            old.right = old.left + 2;
            old.top += 30;
            old.bottom -= 30;
            FillRect(hdc, &old, hbr);
        }
        if (i != first_node_index)
            return;
        LPCWSTR fmt =
            L"这个节点是回收链表的第一个节点\r\n"
            L"如果arr分配完后从这里开始分配\r\n"
            L"当前节点一共占用%d个成员\r\n"
            L"下一个节点: 0x%p\r\n"
            ;

        wchar_t szText[100];
        swprintf_s(szText, fmt, first_node_count, pNext);
        pfn_draw_remark(width, height, 80, szText);
    };
    // 绘画点击的项目
    auto pfn_draw_click = [&]()
    {
        kuodafu::PLIST_NODE pFree = pHead->list;
        kuodafu::PLIST_NODE pNext = 0;
        const int height = 90;
        const int width = 260;

        LPBYTE pItemStart = 0, pItemEnd = 0;
        pool_view.GetItemStartEnd(pHead, pItemStart, pItemEnd);
        LPBYTE pItem = pItemStart + m_index * m_itemSize;

        const bool isAlloc = pool_view.IsAllocated(pHead, pItem);
        const bool isFreeList = pool_view.IsFreeList(pHead, pItem);

        LPCWSTR fmt =
            L"当前节点地址 = 0x%p\r\n"
            L"当前节点 %s\r\n%s"
            ;

        wchar_t szText[100];
        swprintf_s(szText, fmt, pItem,
            (isAlloc ? L"已分配" : L"未分配"),
            (isFreeList ? L"当前节点是已经被回收的地址\r\n" : L"")
            );
        pfn_draw_remark(width, height, 200, szText);
    };
    auto pfn_draw_block = [&](int i, bool isDraw)
    {
        //const bool isDrawFreeList = (i >= first_node_index && i < first_node_index + first_node_count);
        //if (isDrawFreeList)
            pfn_draw_freelist(i);
        if (i == m_index)
            pfn_draw_click();
        if (!isDraw)
        {
            if (i > first_arr_index)
            {
                RECT old = rcBlock;
                old.left -= 1;
                old.right = old.left + 2;
                old.top += 30;
                old.bottom -= 30;
                FillRect(hdc, &old, hbr);
            }
            return;
        }

        const int height = 60;
        const int width = 280;
        LPCWSTR pszText =
            L"这个节点就是下一个分配出去的地址\r\n"
            L"内存池优先从这里开始分配\r\n"
            ;
        pfn_draw_remark(width, height, 10, pszText);

    };



    

    rcBlock.right = rcBlock.left + width;
    for(int i = 0; i < m_count; i++)
    {
        if (rcBlock.left >= cxClient)
            break;
        wchar_t sz[32];
        LPBYTE pAddr = pItemStart + i * m_itemSize;
        bool isAlloc = pool_view.IsAllocated(pHead, pAddr);
        const bool isFreeList = pool_view.IsFreeList(pHead, pAddr);

        swprintf_s(sz, L"0x%p", pAddr);
        m_rcBlock[i] = rcBlock;
        hbr = isFreeList ? hbrFreeList : (isAlloc ? hbrAllocated : hbrBlock);

        const int isDraw = pAddr == pHead->item;

        draw_block(hdc, rcBlock, sz, hbr, pfn_draw_block, i, isDraw);

    }


    DeleteObject(hbrBlock);
    DeleteObject(hbrAllocated);
    DeleteObject(hbrFreeList);

}
