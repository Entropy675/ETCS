#ifndef BASE_WINDOW_H__
#define BASE_WINDOW_H__
#include "Window.h"

ETCS_SUPERTYPE_BASE(Window)
{
    ETCS_MAKE_INSTANCE(Window)
    ETCS_DISPATCH_METHOD(void,              CreateWindow,   (const char*, title), (uint32_t, width), (uint32_t, height));
    ETCS_DISPATCH_METHOD(bool,              ShouldClose);
    ETCS_DISPATCH_METHOD(void,              PollEvents);
    ETCS_DISPATCH_METHOD(WindowSize,        GetSize);
    ETCS_DISPATCH_METHOD(WindowPosition,    GetPosition);
    ETCS_DISPATCH_METHOD(void,              SetPosition,    (int32_t, x),         (int32_t, y));
    ETCS_DISPATCH_METHOD(void,              CloseWindow);
};
#endif
