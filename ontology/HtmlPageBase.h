#ifndef BASE_HTMLPAGE_H__
#define BASE_HTMLPAGE_H__
#include "HtmlPage.h"

ETCS_SUPERTYPE_BASE(HtmlPage)
{
    ETCS_MAKE_INSTANCE(HtmlPage)
    ETCS_DISPATCH_METHOD_CONST(HtmlPage_::ResolvedAsset, Resolve, (const std::string&, request_path));
    ETCS_DISPATCH_METHOD_CONST(bool, IsFileBacked);
};

#endif // BASE_HTMLPAGE_H__
