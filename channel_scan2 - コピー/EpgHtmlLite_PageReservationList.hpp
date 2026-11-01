#pragma once
#ifndef EPG_HTML_LITE_PAGE_RESERVATION_LIST_HPP
#define EPG_HTML_LITE_PAGE_RESERVATION_LIST_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "EpgHtmlLite.hpp"

namespace EpgHtmlLite::PageReservationList
{
    bool Generate(const Context& ctx, Result& ioResult);
}

#endif // EPG_HTML_LITE_PAGE_RESERVATION_LIST_HPP