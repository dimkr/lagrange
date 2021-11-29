/* Copyright 2020 Jaakko Keränen <jaakko.keranen@iki.fi>

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. */

#include "sidebarwidget.h"

#include "app.h"
#include "defs.h"
#include "bookmarks.h"
#include "command.h"
#include "documentwidget.h"
#include "feeds.h"
#include "gmcerts.h"
#include "gmutil.h"
#include "gmdocument.h"
#include "inputwidget.h"
#include "labelwidget.h"
#include "listwidget.h"
#include "keys.h"
#include "paint.h"
#include "root.h"
#include "scrollwidget.h"
#include "util.h"
#include "visited.h"

#include <the_Foundation/intset.h>
#include <the_Foundation/regexp.h>
#include <the_Foundation/stringarray.h>
#include <SDL_clipboard.h>
#include <SDL_mouse.h>

iDeclareType(SidebarItem)
typedef iListItemClass iSidebarItemClass;

struct Impl_SidebarItem {
    iListItem listItem;
    uint32_t  id;
    int       indent;
    iChar     icon;
    iBool     isBold;
    iString   label;
    iString   meta;
    iString   url;
};

void init_SidebarItem(iSidebarItem *d) {
    init_ListItem(&d->listItem);
    d->id     = 0;
    d->indent = 0;
    d->icon   = 0;
    d->isBold = iFalse;
    init_String(&d->label);
    init_String(&d->meta);
    init_String(&d->url);
}

void deinit_SidebarItem(iSidebarItem *d) {
    deinit_String(&d->url);
    deinit_String(&d->meta);
    deinit_String(&d->label);
}

static void draw_SidebarItem_(const iSidebarItem *d, iPaint *p, iRect itemRect, const iListWidget *list);

iBeginDefineSubclass(SidebarItem, ListItem)
    .draw = (iAny *) draw_SidebarItem_,
iEndDefineSubclass(SidebarItem)

iDefineObjectConstruction(SidebarItem)

/*----------------------------------------------------------------------------------------------*/

struct Impl_SidebarWidget {
    iWidget           widget;
    enum iSidebarSide side;
    enum iSidebarMode mode;
    enum iFeedsMode   feedsMode;
    iString           cmdPrefix;
    iWidget *         blank;
    iListWidget *     list;
    iWidget *         actions; /* below the list, area for buttons */
    int               modeScroll[max_SidebarMode];
    iLabelWidget *    modeButtons[max_SidebarMode];
    int               maxButtonLabelWidth;
    float             widthAsGaps;
    int               buttonFont;
    int               itemFonts[2];
    size_t            numUnreadEntries;
    iWidget *         resizer;
    iWidget *         menu; /* context menu for an item */
    iWidget *         modeMenu; /* context menu for the sidebar mode (no item) */
    iSidebarItem *    contextItem;  /* list item accessed in the context menu */
    size_t            contextIndex; /* index of list item accessed in the context menu */
    iIntSet *         closedFolders; /* otherwise open */
};

iDefineObjectConstructionArgs(SidebarWidget, (enum iSidebarSide side), side)

static iBool isResizing_SidebarWidget_(const iSidebarWidget *d) {
    return (flags_Widget(d->resizer) & pressed_WidgetFlag) != 0;
}

iBookmark *parent_Bookmark(const iBookmark *d) {
    /* TODO: Parent pointers should be prefetched! */
    if (d->parentId) {
        return get_Bookmarks(bookmarks_App(), d->parentId);
    }
    return NULL;
}

iBool hasParent_Bookmark(const iBookmark *d, uint32_t parentId) {
    /* TODO: Parent pointers should be prefetched! */
    while (d->parentId) {
        if (d->parentId == parentId) {
            return iTrue;
        }
        d = get_Bookmarks(bookmarks_App(), d->parentId);
    }
    return iFalse;
}

int depth_Bookmark(const iBookmark *d) {
    /* TODO: Precalculate this! */
    int depth = 0;
    for (; d->parentId; depth++) {
        d = get_Bookmarks(bookmarks_App(), d->parentId);
    }
    return depth;
}

int cmpTree_Bookmark(const iBookmark **a, const iBookmark **b) {
    const iBookmark *bm1 = *a, *bm2 = *b;
    /* Contents of a parent come after it. */
    if (hasParent_Bookmark(bm2, id_Bookmark(bm1))) {
        return -1;
    }
    if (hasParent_Bookmark(bm1, id_Bookmark(bm2))) {
        return 1;
    }
    /* Comparisons are only valid inside the same parent. */
    while (bm1->parentId != bm2->parentId) {
        int depth1 = depth_Bookmark(bm1);
        int depth2 = depth_Bookmark(bm2);
        if (depth1 != depth2) {
            /* Equalize the depth. */
            while (depth1 > depth2) {
                bm1 = parent_Bookmark(bm1);
                depth1--;
            }
            while (depth2 > depth1) {
                bm2 = parent_Bookmark(bm2);
                depth2--;
            }
            continue;
        }
        bm1 = parent_Bookmark(bm1);
        depth1--;
        bm2 = parent_Bookmark(bm2);
        depth2--;
    }
    const int cmp = iCmp(bm1->order, bm2->order);
    if (cmp) return cmp;
    return cmpStringCase_String(&bm1->title, &bm2->title);
}

static iLabelWidget *addActionButton_SidebarWidget_(iSidebarWidget *d, const char *label,
                                                    const char *command, int64_t flags) {
    iLabelWidget *btn = addChildFlags_Widget(d->actions,
                                             iClob(new_LabelWidget(label, command)),
                                             //(deviceType_App() != desktop_AppDeviceType ?
                                             // extraPadding_WidgetFlag : 0) |
                                             flags);
    setFont_LabelWidget(btn, deviceType_App() == phone_AppDeviceType && d->side == right_SidebarSide
                                 ? uiLabelBig_FontId
                                 : d->buttonFont);
    checkIcon_LabelWidget(btn);
    return btn;
}

static iGmIdentity *menuIdentity_SidebarWidget_(const iSidebarWidget *d) {
    if (d->mode == identities_SidebarMode) {
        if (d->contextItem) {
            return identity_GmCerts(certs_App(), d->contextItem->id);
        }
    }
    return NULL;
}

static void updateContextMenu_SidebarWidget_(iSidebarWidget *d) {
    if (d->mode != identities_SidebarMode) {
        return;
    }
    iArray *items = collectNew_Array(sizeof(iMenuItem));
    pushBackN_Array(items, (iMenuItem[]){
        { person_Icon " ${ident.use}", 0, 0, "ident.use arg:1" },
        { close_Icon " ${ident.stopuse}", 0, 0, "ident.use arg:0" },
        { close_Icon " ${ident.stopuse.all}", 0, 0, "ident.use arg:0 clear:1" },
        { "---", 0, 0, NULL },
        { edit_Icon " ${menu.edit.notes}", 0, 0, "ident.edit" },
        { "${ident.fingerprint}", 0, 0, "ident.fingerprint" },
        { export_Icon " ${ident.export}", 0, 0, "ident.export" },
        { "---", 0, 0, NULL },
        { delete_Icon " " uiTextCaution_ColorEscape "${ident.delete}", 0, 0, "ident.delete confirm:1" },
    }, 9);
    /* Used URLs. */
    const iGmIdentity *ident = menuIdentity_SidebarWidget_(d);
    if (ident) {
        size_t insertPos = 3;
        if (!isEmpty_StringSet(ident->useUrls)) {
            insert_Array(items, insertPos++, &(iMenuItem){ "---", 0, 0, NULL });
        }
        const iString *docUrl = url_DocumentWidget(document_App());
        iBool usedOnCurrentPage = iFalse;
        iConstForEach(StringSet, i, ident->useUrls) {            
            const iString *url = i.value;
            usedOnCurrentPage |= equalCase_String(docUrl, url);
            iRangecc urlStr = range_String(url);
            if (startsWith_Rangecc(urlStr, "gemini://")) {
                urlStr.start += 9; /* omit the default scheme */
            }
            if (endsWith_Rangecc(urlStr, "/")) {
                urlStr.end--; /* looks cleaner */
            }
            insert_Array(items,
                         insertPos++,
                         &(iMenuItem){ format_CStr(globe_Icon " %s", cstr_Rangecc(urlStr)),
                                       0,
                                       0,
                                       format_CStr("!open url:%s", cstr_String(url)) });
        }
        if (!usedOnCurrentPage) {
            remove_Array(items, 1);
        }
    }
    destroy_Widget(d->menu);    
    d->menu = makeMenu_Widget(as_Widget(d), data_Array(items), size_Array(items));    
}

static iBool isBookmarkFolded_SidebarWidget_(const iSidebarWidget *d, const iBookmark *bm) {
    while (bm->parentId) {
        if (contains_IntSet(d->closedFolders, bm->parentId)) {
            return iTrue;
        }
        bm = get_Bookmarks(bookmarks_App(), bm->parentId);
    }
    return iFalse;
}

static void updateItems_SidebarWidget_(iSidebarWidget *d) {
    clear_ListWidget(d->list);
    releaseChildren_Widget(d->blank);
    releaseChildren_Widget(d->actions);
    d->actions->rect.size.y = 0;
    destroy_Widget(d->menu);
    destroy_Widget(d->modeMenu);
    d->menu       = NULL;
    d->modeMenu   = NULL;
    iBool isEmpty = iFalse; /* show blank? */
    switch (d->mode) {
        case feeds_SidebarMode: {
            const iString *docUrl = canonicalUrl_String(url_DocumentWidget(document_App()));
                                    /* TODO: internal URI normalization */
            iTime now;
            iDate on;
            initCurrent_Time(&now);
            init_Date(&on, &now);
            const iDate today = on;
            iZap(on);
            size_t numItems = 0;
            isEmpty = iTrue;
            iConstForEach(PtrArray, i, listEntries_Feeds()) {
                const iFeedEntry *entry = i.ptr;
                if (isHidden_FeedEntry(entry)) {
                    continue; /* A hidden entry. */
                }
                /* Don't show entries in the far future. */
                if (secondsSince_Time(&now, &entry->posted) < -24 * 60 * 60) {
                    continue;
                }
                /* Exclude entries that are too old for Visited to keep track of. */
                if (secondsSince_Time(&now, &entry->discovered) > maxAge_Visited) {
                    break; /* the rest are even older */
                }
                isEmpty = iFalse;
                const iBool isOpen = equal_String(docUrl, &entry->url);
                const iBool isUnread = isUnread_FeedEntry(entry);
                if (d->feedsMode == unread_FeedsMode && !isUnread && !isOpen) {
                    continue;
                }
                /* Insert date separators. */ {
                    iDate entryDate;
                    init_Date(&entryDate, &entry->posted);
                    if (on.year != entryDate.year || on.month != entryDate.month ||
                        on.day != entryDate.day) {
                        on = entryDate;
                        iSidebarItem *sep = new_SidebarItem();
                        sep->listItem.isSeparator = iTrue;
                        iString *text = format_Date(&on,
                                                    cstr_Lang(on.year == today.year
                                                                  ? "sidebar.date.thisyear"
                                                                  : "sidebar.date.otheryear"));
                        if (today.year == on.year &&
                            today.month == on.month &&
                            today.day == on.day) {
                            appendCStr_String(text, " \u2014 ");
                            appendCStr_String(text, cstr_Lang("feeds.today"));
                        }
                        set_String(&sep->meta, text);
                        delete_String(text);
                        addItem_ListWidget(d->list, sep);
                        iRelease(sep);
                    }
                }
                iSidebarItem *item = new_SidebarItem();
                item->listItem.isSelected = isOpen; /* currently being viewed */
                item->indent = isUnread;
                set_String(&item->url, &entry->url);
                set_String(&item->label, &entry->title);
                const iBookmark *bm = get_Bookmarks(bookmarks_App(), entry->bookmarkId);
                if (bm) {
                    item->id = entry->bookmarkId;
                    item->icon = bm->icon;
                    append_String(&item->meta, &bm->title);
                }
                addItem_ListWidget(d->list, item);
                iRelease(item);
                if (++numItems == 100) {
                    /* For more items, one can always see "about:feeds". A large number of items
                       is a bit difficult to navigate in the sidebar. */
                    break;
                }
            }
            /* Actions. */ {
                addActionButton_SidebarWidget_(
                    d, check_Icon " ${feeds.markallread}", "feeds.markallread", expand_WidgetFlag |
                    tight_WidgetFlag);
                updateSize_LabelWidget(addChildFlags_Widget(d->actions,
                                     iClob(new_LabelWidget("${sidebar.action.show}", NULL)),
                                                            frameless_WidgetFlag | tight_WidgetFlag));
                const iMenuItem items[] = {
                    { "${sidebar.action.feeds.showall}", SDLK_u, KMOD_SHIFT, "feeds.mode arg:0" },
                    { "${sidebar.action.feeds.showunread}", SDLK_u, 0, "feeds.mode arg:1" },
                };
                iWidget *dropButton = addChild_Widget(
                    d->actions,
                    iClob(makeMenuButton_LabelWidget(items[d->feedsMode].label, items, 2)));
                setFixedSize_Widget(
                    dropButton,
                    init_I2(iMaxi(20 * gap_UI, measure_Text(
                                default_FontId,
                                translateCStr_Lang(items[findWidestLabel_MenuItem(items, 2)].label))
                                    .advance.x +
                                                   6 * gap_UI),
                            -1));
            }
            d->menu = makeMenu_Widget(
                as_Widget(d),
                (iMenuItem[]){ { openTab_Icon " ${feeds.entry.newtab}", 0, 0, "feed.entry.opentab" },
                               { circle_Icon " ${feeds.entry.markread}", 0, 0, "feed.entry.toggleread" },
                               { bookmark_Icon " ${feeds.entry.bookmark}", 0, 0, "feed.entry.bookmark" },
                               { "---", 0, 0, NULL },
                               { page_Icon " ${feeds.entry.openfeed}", 0, 0, "feed.entry.openfeed" },
                               { edit_Icon " ${feeds.edit}", 0, 0, "feed.entry.edit" },
                               { whiteStar_Icon " " uiTextCaution_ColorEscape "${feeds.unsubscribe}", 0, 0, "feed.entry.unsubscribe" },
                               { "---", 0, 0, NULL },
                               { check_Icon " ${feeds.markallread}", SDLK_a, KMOD_SHIFT, "feeds.markallread" },
                               { reload_Icon " ${feeds.refresh}", SDLK_r, KMOD_PRIMARY | KMOD_SHIFT, "feeds.refresh" } },
                10);
            d->modeMenu = makeMenu_Widget(
                as_Widget(d),
                (iMenuItem[]){
                    { check_Icon " ${feeds.markallread}", SDLK_a, KMOD_SHIFT, "feeds.markallread" },
                    { reload_Icon " ${feeds.refresh}", SDLK_r, KMOD_PRIMARY | KMOD_SHIFT, "feeds.refresh" } },
                2);
            break;
        }
        case documentOutline_SidebarMode: {
            const iGmDocument *doc = document_DocumentWidget(document_App());
            iConstForEach(Array, i, headings_GmDocument(doc)) {
                const iGmHeading *head = i.value;
                iSidebarItem *item = new_SidebarItem();
                item->id = index_ArrayConstIterator(&i);
                setRange_String(&item->label, head->text);
                item->indent = head->level * 5 * gap_UI;
                item->isBold = head->level == 0;
                addItem_ListWidget(d->list, item);
                iRelease(item);
            }
            break;
        }
        case bookmarks_SidebarMode: {
            iRegExp *homeTag         = iClob(new_RegExp("\\b" homepage_BookmarkTag "\\b", caseSensitive_RegExpOption));
            iRegExp *subTag          = iClob(new_RegExp("\\b" subscribed_BookmarkTag "\\b", caseSensitive_RegExpOption));
            iRegExp *remoteSourceTag = iClob(new_RegExp("\\b" remoteSource_BookmarkTag "\\b", caseSensitive_RegExpOption));
            iRegExp *remoteTag       = iClob(new_RegExp("\\b" remote_BookmarkTag "\\b", caseSensitive_RegExpOption));
            iRegExp *linkSplitTag    = iClob(new_RegExp("\\b" linkSplit_BookmarkTag "\\b", caseSensitive_RegExpOption));
            iConstForEach(PtrArray, i, list_Bookmarks(bookmarks_App(), cmpTree_Bookmark, NULL, NULL)) {
                const iBookmark *bm = i.ptr;
                if (isBookmarkFolded_SidebarWidget_(d, bm)) {
                    continue; /* inside a closed folder */
                }
                iSidebarItem *item = new_SidebarItem();
                item->listItem.isDraggable = iTrue;
                item->isBold = item->listItem.isDropTarget = isFolder_Bookmark(bm);
                item->id = id_Bookmark(bm);
                item->indent = depth_Bookmark(bm);
                if (isFolder_Bookmark(bm)) {
                    item->icon = contains_IntSet(d->closedFolders, item->id) ? 0x27e9 : 0xfe40;
                }
                else {
                    item->icon = bm->icon;
                }
                set_String(&item->url, &bm->url);
                set_String(&item->label, &bm->title);
                /* Icons for special tags. */ {
                    iRegExpMatch m;
                    init_RegExpMatch(&m);
                    if (matchString_RegExp(subTag, &bm->tags, &m)) {
                        appendChar_String(&item->meta, 0x2605);
                    }
                    init_RegExpMatch(&m);
                    if (matchString_RegExp(homeTag, &bm->tags, &m)) {
                        appendChar_String(&item->meta, 0x1f3e0);
                    }
                    init_RegExpMatch(&m);
                    if (matchString_RegExp(remoteTag, &bm->tags, &m)) {
                        item->listItem.isDraggable = iFalse;
                    }
                    init_RegExpMatch(&m);
                    if (matchString_RegExp(remoteSourceTag, &bm->tags, &m)) {
                        appendChar_String(&item->meta, 0x2913);
                        item->isBold = iTrue;
                    }
                    init_RegExpMatch(&m);
                    if (matchString_RegExp(linkSplitTag, &bm->tags, &m)) {
                        appendChar_String(&item->meta, 0x25e7);
                    }
                }
                addItem_ListWidget(d->list, item);
                iRelease(item);
            }
            d->menu = makeMenu_Widget(
                as_Widget(d),
                (iMenuItem[]){ { openTab_Icon " ${menu.opentab}", 0, 0, "bookmark.open newtab:1" },
                               { openTabBg_Icon " ${menu.opentab.background}", 0, 0, "bookmark.open newtab:2" },
                               { "---", 0, 0, NULL },
                               { edit_Icon " ${menu.edit}", 0, 0, "bookmark.edit" },
                               { copy_Icon " ${menu.dup}", 0, 0, "bookmark.dup" },
                               { "${menu.copyurl}", 0, 0, "bookmark.copy" },
                               { "---", 0, 0, NULL },
                               { "", 0, 0, "bookmark.tag tag:subscribed" },
                               { "", 0, 0, "bookmark.tag tag:homepage" },
                               { "", 0, 0, "bookmark.tag tag:remotesource" },
                               { "---", 0, 0, NULL },
                               { delete_Icon " " uiTextCaution_ColorEscape "${bookmark.delete}", 0, 0, "bookmark.delete" },
                               { "---", 0, 0, NULL },
                               { add_Icon " ${menu.newfolder}", 0, 0, "bookmark.addfolder" },
                               { upDownArrow_Icon " ${menu.sort.alpha}", 0, 0, "bookmark.sortfolder" },
                               { "---", 0, 0, NULL },
                               { reload_Icon " ${bookmarks.reload}", 0, 0, "bookmarks.reload.remote" } },
               17);
            d->modeMenu = makeMenu_Widget(
                as_Widget(d),
                (iMenuItem[]){ { bookmark_Icon " ${menu.page.bookmark}", SDLK_d, KMOD_PRIMARY, "bookmark.add" },
                               { add_Icon " ${menu.newfolder}", 0, 0, "bookmark.addfolder" },
                               { "---", 0, 0, NULL },                               
                               { upDownArrow_Icon " ${menu.sort.alpha}", 0, 0, "bookmark.sortfolder" },
                               { "---", 0, 0, NULL },
                               { reload_Icon " ${bookmarks.reload}", 0, 0, "bookmarks.reload.remote" } },               
                6);
            break;
        }
        case history_SidebarMode: {
            iDate on;
            initCurrent_Date(&on);
            const int thisYear = on.year;
            iConstForEach(PtrArray, i, list_Visited(visited_App(), 200)) {
                const iVisitedUrl *visit = i.ptr;
                iSidebarItem *item = new_SidebarItem();
                set_String(&item->url, &visit->url);
                set_String(&item->label, &visit->url);
                if (prefs_App()->decodeUserVisibleURLs) {
                    urlDecodePath_String(&item->label);
                }
                else {
                    urlEncodePath_String(&item->label);
                }
                iDate date;
                init_Date(&date, &visit->when);
                if (date.day != on.day || date.month != on.month || date.year != on.year) {
                    on = date;
                    /* Date separator. */
                    iSidebarItem *sep = new_SidebarItem();
                    sep->listItem.isSeparator = iTrue;
                    const iString *text = collect_String(
                        format_Date(&date,
                                    cstr_Lang(date.year != thisYear ? "sidebar.date.otheryear"
                                                                    : "sidebar.date.thisyear")));
                    set_String(&sep->meta, text);
                    const int yOffset = itemHeight_ListWidget(d->list) * 2 / 3;
                    sep->id = yOffset;
                    addItem_ListWidget(d->list, sep);
                    iRelease(sep);
                    /* Date separators are two items tall. */
                    sep = new_SidebarItem();
                    sep->listItem.isSeparator = iTrue;
                    sep->id = -itemHeight_ListWidget(d->list) + yOffset;
                    set_String(&sep->meta, text);
                    addItem_ListWidget(d->list, sep);
                    iRelease(sep);
                }
                addItem_ListWidget(d->list, item);
                iRelease(item);
            }
            d->menu = makeMenu_Widget(
                as_Widget(d),
                (iMenuItem[]){
                    { "${menu.copyurl}", 0, 0, "history.copy" },
                    { bookmark_Icon " ${sidebar.entry.bookmark}", 0, 0, "history.addbookmark" },
                    { "---", 0, 0, NULL },
                    { close_Icon " ${menu.forgeturl}", 0, 0, "history.delete" },
                    { "---", 0, 0, NULL },
                    { delete_Icon " " uiTextCaution_ColorEscape "${history.clear}", 0, 0, "history.clear confirm:1" },
                }, 6);
            d->modeMenu = makeMenu_Widget(
                as_Widget(d),
                (iMenuItem[]){
                    { delete_Icon " " uiTextCaution_ColorEscape "${history.clear}", 0, 0, "history.clear confirm:1" },
                }, 1);
            break;
        }
        case identities_SidebarMode: {
            const iString *tabUrl = url_DocumentWidget(document_App());
            const iRangecc tabHost = urlHost_String(tabUrl);
            isEmpty = iTrue;
            iConstForEach(PtrArray, i, identities_GmCerts(certs_App())) {
                const iGmIdentity *ident = i.ptr;
                iSidebarItem *item = new_SidebarItem();
                item->id = (uint32_t) index_PtrArrayConstIterator(&i);
                item->icon = 0x1f464; /* person */
                set_String(&item->label, name_GmIdentity(ident));
                iDate until;
                validUntil_TlsCertificate(ident->cert, &until);
                const iBool isActive = isUsedOn_GmIdentity(ident, tabUrl);
                format_String(&item->meta,
                              "%s",
                              isActive ? cstr_Lang("ident.using")
                              : isUsed_GmIdentity(ident)
                                  ? formatCStrs_Lang("ident.usedonurls.n", size_StringSet(ident->useUrls))
                                  : cstr_Lang("ident.notused"));
                const char *expiry =
                    ident->flags & temporary_GmIdentityFlag
                        ? cstr_Lang("ident.temporary")
                        : cstrCollect_String(format_Date(&until, cstr_Lang("ident.expiry")));
                if (isEmpty_String(&ident->notes)) {
                    appendFormat_String(&item->meta, "\n%s", expiry);
                }
                else {
                    appendFormat_String(&item->meta,
                                        " \u2014 %s\n%s%s",
                                        expiry,
                                        escape_Color(uiHeading_ColorId),
                                        cstr_String(&ident->notes));
                }
                item->listItem.isSelected = isActive;
                if (isUsedOnDomain_GmIdentity(ident, tabHost)) {
                    item->indent = 1; /* will be highlighted */
                }
                addItem_ListWidget(d->list, item);
                iRelease(item);
                isEmpty = iFalse;
            }
            /* Actions. */
            if (!isEmpty) {
                addActionButton_SidebarWidget_(d, add_Icon " ${sidebar.action.ident.new}", "ident.new", 0);
                addActionButton_SidebarWidget_(d, "${sidebar.action.ident.import}", "ident.import", 0);
            }
            break;
        }
        default:
            break;
    }
    scrollOffset_ListWidget(d->list, 0);
    updateVisible_ListWidget(d->list);
    invalidate_ListWidget(d->list);
    /* Content for a blank tab. */
    if (isEmpty) {
        if (d->mode == feeds_SidebarMode) {
            iWidget *div = makeVDiv_Widget();
            setPadding_Widget(div, 3 * gap_UI, 0, 3 * gap_UI, 2 * gap_UI);
            addChildFlags_Widget(div, iClob(new_Widget()), expand_WidgetFlag); /* pad */
            addChild_Widget(div, iClob(new_LabelWidget("${menu.feeds.refresh}", "feeds.refresh")));
            addChildFlags_Widget(div, iClob(new_Widget()), expand_WidgetFlag); /* pad */
            addChild_Widget(d->blank, iClob(div));
        }
        else if (d->mode == identities_SidebarMode) {
            iWidget *div = makeVDiv_Widget();
            setPadding_Widget(div, 3 * gap_UI, 0, 3 * gap_UI, 2 * gap_UI);
            addChildFlags_Widget(div, iClob(new_Widget()), expand_WidgetFlag); /* pad */
            iLabelWidget *msg = new_LabelWidget("${sidebar.empty.idents}", NULL);
            setFont_LabelWidget(msg, uiLabelLarge_FontId);
            addChildFlags_Widget(div, iClob(msg), frameless_WidgetFlag);
            addChild_Widget(div, iClob(makePadding_Widget(3 * gap_UI)));
            addChild_Widget(div, iClob(new_LabelWidget("${menu.identity.new}", "ident.new")));
            addChild_Widget(div, iClob(makePadding_Widget(gap_UI)));
            addChild_Widget(div, iClob(new_LabelWidget("${menu.identity.import}", "ident.import")));
            addChildFlags_Widget(div, iClob(new_Widget()), expand_WidgetFlag); /* pad */
            iLabelWidget *linkLabel;
            setBackgroundColor_Widget(
                addChildFlags_Widget(
                    div,
                    iClob(linkLabel = new_LabelWidget(format_CStr(cstr_Lang("ident.gotohelp"),
                                                      uiTextStrong_ColorEscape,
                                                      restore_ColorEscape),
                                          "!open newtab:1 gotoheading:1.6 url:about:help")),
                    frameless_WidgetFlag | fixedHeight_WidgetFlag),
                uiBackgroundSidebar_ColorId);
            setWrap_LabelWidget(linkLabel, iTrue);
            addChild_Widget(d->blank, iClob(div));
        }
//        arrange_Widget(d->blank);
    }
#if 0
    if (deviceType_App() != desktop_AppDeviceType) {
        /* Touch-friendly action buttons. */
        iForEach(ObjectList, i, children_Widget(d->actions)) {
            if (isInstance_Object(i.object, &Class_LabelWidget)) {
                setPadding_Widget(i.object, 0, gap_UI, 0, gap_UI);
            }
        }
    }
#endif
    arrange_Widget(d->actions);
    arrange_Widget(as_Widget(d));
    updateMouseHover_ListWidget(d->list);
}

static size_t findItem_SidebarWidget_(const iSidebarWidget *d, int id) {
    /* Note that this is O(n), so only meant for infrequent use. */
    for (size_t i = 0; i < numItems_ListWidget(d->list); i++) {
        const iSidebarItem *item = constItem_ListWidget(d->list, i);
        if (item->id == id) {
            return i;
        }
    }
    return iInvalidPos;
}

static void updateItemHeight_SidebarWidget_(iSidebarWidget *d) {
    if (d->list) {
        const float heights[max_SidebarMode] = { 1.333f, 2.333f, 1.333f, 3.5f, 1.2f };
        setItemHeight_ListWidget(d->list, heights[d->mode] * lineHeight_Text(d->itemFonts[0]));
    }
}

iBool setMode_SidebarWidget(iSidebarWidget *d, enum iSidebarMode mode) {
    if (d->mode == mode) {
        return iFalse;
    }
    if (d->mode >= 0 && d->mode < max_SidebarMode) {
        d->modeScroll[d->mode] = scrollPos_ListWidget(d->list); /* saved for later */
    }
    d->mode = mode;
    for (enum iSidebarMode i = 0; i < max_SidebarMode; i++) {
        setFlags_Widget(as_Widget(d->modeButtons[i]), selected_WidgetFlag, i == d->mode);
    }
    setBackgroundColor_Widget(as_Widget(d->list),
                              d->mode == documentOutline_SidebarMode ? tmBannerBackground_ColorId
                                                                     : uiBackgroundSidebar_ColorId);
    updateItemHeight_SidebarWidget_(d);
    /* Restore previous scroll position. */
    setScrollPos_ListWidget(d->list, d->modeScroll[mode]);
    return iTrue;
}

void setClosedFolders_SidebarWidget(iSidebarWidget *d, const iIntSet *closedFolders) {
    delete_IntSet(d->closedFolders);
    d->closedFolders = copy_IntSet(closedFolders);
}

enum iSidebarMode mode_SidebarWidget(const iSidebarWidget *d) {
    return d ? d->mode : 0;
}

enum iFeedsMode feedsMode_SidebarWidget(const iSidebarWidget *d) {
    return d ? d->feedsMode : 0;
}

float width_SidebarWidget(const iSidebarWidget *d) {
    return d ? d->widthAsGaps : 0;
}

const iIntSet *closedFolders_SidebarWidget(const iSidebarWidget *d) {
    return d->closedFolders;
}

static const char *normalModeLabels_[max_SidebarMode] = {
    book_Icon   " ${sidebar.bookmarks}",
    star_Icon   " ${sidebar.feeds}",
    clock_Icon  " ${sidebar.history}",
    person_Icon " ${sidebar.identities}",
    page_Icon   " ${sidebar.outline}",
};

static const char *tightModeLabels_[max_SidebarMode] = {
    book_Icon,
    star_Icon,
    clock_Icon,
    person_Icon,
    page_Icon,
};

const char *icon_SidebarMode(enum iSidebarMode mode) {
    return tightModeLabels_[mode];
}

static void updateMetrics_SidebarWidget_(iSidebarWidget *d) {
    if (d->resizer) {
        d->resizer->rect.size.x = gap_UI;
    }
    d->maxButtonLabelWidth = 0;
    for (int i = 0; i < max_SidebarMode; i++) {
        if (d->modeButtons[i]) {
            d->maxButtonLabelWidth =
                iMaxi(d->maxButtonLabelWidth,
                      3 * gap_UI + measure_Text(font_LabelWidget(d->modeButtons[i]),
                                                translateCStr_Lang(normalModeLabels_[i]))
                                       .bounds.size.x);
        }
    }
    updateItemHeight_SidebarWidget_(d);
}

void init_SidebarWidget(iSidebarWidget *d, enum iSidebarSide side) {
    iWidget *w = as_Widget(d);
    init_Widget(w);
    setId_Widget(w, side == left_SidebarSide ? "sidebar" : "sidebar2");
    initCopy_String(&d->cmdPrefix, id_Widget(w));
    appendChar_String(&d->cmdPrefix, '.');
    setBackgroundColor_Widget(w, none_ColorId);
    setFlags_Widget(w,
                    collapse_WidgetFlag | hidden_WidgetFlag | arrangeHorizontal_WidgetFlag |
                        resizeWidthOfChildren_WidgetFlag | noFadeBackground_WidgetFlag |
                    noShadowBorder_WidgetFlag,
                    iTrue);
    iZap(d->modeScroll);
    d->side = side;
    d->mode = -1;
    d->feedsMode = all_FeedsMode;
    d->numUnreadEntries = 0;
    d->buttonFont = uiLabel_FontId; /* wiil be changed later */
    d->itemFonts[0] = uiContent_FontId;
    d->itemFonts[1] = uiContentBold_FontId;
#if defined (iPlatformMobile)
    if (deviceType_App() == phone_AppDeviceType) {
        d->itemFonts[0] = uiLabelBig_FontId;
        d->itemFonts[1] = uiLabelBigBold_FontId;
    }
    d->widthAsGaps = 73.0f;
#else
    d->widthAsGaps = 60.0f;
#endif
    setFlags_Widget(w, fixedWidth_WidgetFlag, iTrue);
    iWidget *vdiv = makeVDiv_Widget();
    addChildFlags_Widget(w, vdiv, resizeToParentWidth_WidgetFlag | resizeToParentHeight_WidgetFlag);
    iZap(d->modeButtons);
    d->resizer = NULL;
    d->list = NULL;
    d->actions = NULL;
    d->closedFolders = new_IntSet();
    /* On a phone, the right sidebar is used exclusively for Identities. */
    const iBool isPhone = deviceType_App() == phone_AppDeviceType;
    if (!isPhone || d->side == left_SidebarSide) {
        iWidget *buttons = new_Widget();        
        setId_Widget(buttons, "buttons");
        setDrawBufferEnabled_Widget(buttons, iTrue);
        for (int i = 0; i < max_SidebarMode; i++) {
            if (deviceType_App() == phone_AppDeviceType && i == identities_SidebarMode) {
                continue;
            }
            d->modeButtons[i] = addChildFlags_Widget(
                buttons,
                iClob(new_LabelWidget(
                    tightModeLabels_[i],
                    format_CStr("%s.mode arg:%d", cstr_String(id_Widget(w)), i))),
                    frameless_WidgetFlag | noBackground_WidgetFlag);
        }
        setButtonFont_SidebarWidget(d, isPhone ? uiLabelBig_FontId : uiLabel_FontId);
        addChildFlags_Widget(vdiv,
                             iClob(buttons),
                             arrangeHorizontal_WidgetFlag |
                                 resizeWidthOfChildren_WidgetFlag |
                             arrangeHeight_WidgetFlag | resizeToParentWidth_WidgetFlag); // |
//                             drawBackgroundToHorizontalSafeArea_WidgetFlag);
        setBackgroundColor_Widget(buttons, uiBackgroundSidebar_ColorId);
    }
    else {
        iLabelWidget *heading = new_LabelWidget(person_Icon " ${sidebar.identities}", NULL);
        checkIcon_LabelWidget(heading);
        setBackgroundColor_Widget(as_Widget(heading), uiBackgroundSidebar_ColorId);
        setTextColor_LabelWidget(heading, uiTextSelected_ColorId);
        setFont_LabelWidget(addChildFlags_Widget(vdiv, iClob(heading), borderTop_WidgetFlag |
                                                 alignLeft_WidgetFlag | frameless_WidgetFlag |
                                                 drawBackgroundToHorizontalSafeArea_WidgetFlag),
                            uiLabelLargeBold_FontId);
    }
    iWidget *content = new_Widget();
    setFlags_Widget(content, resizeChildren_WidgetFlag, iTrue);
    iWidget *listAndActions = makeVDiv_Widget();
    addChild_Widget(content, iClob(listAndActions));
    d->list = new_ListWidget();
    setPadding_Widget(as_Widget(d->list), 0, gap_UI, 0, gap_UI);
    addChildFlags_Widget(listAndActions,
                         iClob(d->list),
                         expand_WidgetFlag); // | drawBackgroundToHorizontalSafeArea_WidgetFlag);
    setId_Widget(addChildPosFlags_Widget(listAndActions,
                                         iClob(d->actions = new_Widget()),
                                         isPhone ? front_WidgetAddPos : back_WidgetAddPos,
                                         arrangeHorizontal_WidgetFlag | arrangeHeight_WidgetFlag |
                                         resizeWidthOfChildren_WidgetFlag), // |
//                                             drawBackgroundToHorizontalSafeArea_WidgetFlag),
                 "actions");
    setBackgroundColor_Widget(d->actions, uiBackgroundSidebar_ColorId);
    d->contextItem = NULL;
    d->contextIndex = iInvalidPos;
    d->blank = new_Widget();
    addChildFlags_Widget(content, iClob(d->blank), resizeChildren_WidgetFlag);
    addChildFlags_Widget(vdiv, iClob(content), expand_WidgetFlag);
    setMode_SidebarWidget(d,
                          deviceType_App() == phone_AppDeviceType && d->side == right_SidebarSide ?
                          identities_SidebarMode : bookmarks_SidebarMode);
    d->resizer =
        addChildFlags_Widget(w,
                             iClob(new_Widget()),
                             hover_WidgetFlag | commandOnClick_WidgetFlag | fixedWidth_WidgetFlag |
                                 resizeToParentHeight_WidgetFlag |
                                 (side == left_SidebarSide ? moveToParentRightEdge_WidgetFlag
                                                           : moveToParentLeftEdge_WidgetFlag));
    if (deviceType_App() == phone_AppDeviceType) {
        setFlags_Widget(d->resizer, hidden_WidgetFlag | disabled_WidgetFlag, iTrue);
    }
    setId_Widget(d->resizer, side == left_SidebarSide ? "sidebar.grab" : "sidebar2.grab");
    setBackgroundColor_Widget(d->resizer, none_ColorId);
    d->menu     = NULL;
    d->modeMenu = NULL;
    addAction_Widget(w, SDLK_r, KMOD_PRIMARY | KMOD_SHIFT, "feeds.refresh");
    updateMetrics_SidebarWidget_(d);
    if (side == left_SidebarSide) {
        postCommand_App("~sidebar.update"); /* unread count */
    }
}

void deinit_SidebarWidget(iSidebarWidget *d) {
    deinit_String(&d->cmdPrefix);
    delete_IntSet(d->closedFolders);
}

iBool setButtonFont_SidebarWidget(iSidebarWidget *d, int font) {
    if (d->buttonFont != font) {
        d->buttonFont = font;
        for (int i = 0; i < max_SidebarMode; i++) {
            if (d->modeButtons[i]) {
                setFont_LabelWidget(d->modeButtons[i], font);
            }
        }
        updateMetrics_SidebarWidget_(d);
        return iTrue;
    }
    return iFalse;
}

static const iGmIdentity *constHoverIdentity_SidebarWidget_(const iSidebarWidget *d) {
    if (d->mode == identities_SidebarMode) {
        const iSidebarItem *hoverItem = constHoverItem_ListWidget(d->list);
        if (hoverItem) {
            return identity_GmCerts(certs_App(), hoverItem->id);
        }
    }
    return NULL;
}

static iGmIdentity *hoverIdentity_SidebarWidget_(const iSidebarWidget *d) {
    return iConstCast(iGmIdentity *, constHoverIdentity_SidebarWidget_(d));
}

static void itemClicked_SidebarWidget_(iSidebarWidget *d, iSidebarItem *item, size_t itemIndex) {
    setFocus_Widget(NULL);
    switch (d->mode) {
        case documentOutline_SidebarMode: {
            const iGmDocument *doc = document_DocumentWidget(document_App());
            if (item->id < size_Array(headings_GmDocument(doc))) {
                const iGmHeading *head = constAt_Array(headings_GmDocument(doc), item->id);
                postCommandf_App("document.goto loc:%p", head->text.start);
                dismissPortraitPhoneSidebars_Root(as_Widget(d)->root);
                setOpenedFromSidebar_DocumentWidget(document_App(), iTrue);
            }
            break;
        }
        case feeds_SidebarMode: {
            postCommandString_Root(get_Root(),
                feedEntryOpenCommand_String(&item->url, openTabMode_Sym(modState_Keys())));
            break;
        }
        case bookmarks_SidebarMode:
            if (isEmpty_String(&item->url)) /* a folder */ {
                if (contains_IntSet(d->closedFolders, item->id)) {
                    remove_IntSet(d->closedFolders, item->id);
                    setRecentFolder_Bookmarks(bookmarks_App(), item->id);
                }
                else {
                    insert_IntSet(d->closedFolders, item->id);
                    setRecentFolder_Bookmarks(bookmarks_App(), 0);
                }
                updateItems_SidebarWidget_(d);
                break;
            }
            /* fall through */
        case history_SidebarMode: {
            if (!isEmpty_String(&item->url)) {
                postCommandf_Root(get_Root(), "open fromsidebar:1 newtab:%d url:%s",
                                 openTabMode_Sym(modState_Keys()),
                                 cstr_String(&item->url));
            }
            break;
        }
        case identities_SidebarMode: {
            d->contextItem  = item;
            if (d->contextIndex != iInvalidPos) {
                invalidateItem_ListWidget(d->list, d->contextIndex);
            }
            d->contextIndex = itemIndex;
            if (itemIndex < numItems_ListWidget(d->list)) {
                updateContextMenu_SidebarWidget_(d);
                arrange_Widget(d->menu);
                openMenu_Widget(d->menu,
                                d->side == left_SidebarSide
                                    ? topRight_Rect(itemRect_ListWidget(d->list, itemIndex))
                                    : addX_I2(topLeft_Rect(itemRect_ListWidget(d->list, itemIndex)),
                                              -width_Widget(d->menu)));
            }
            break;
        }
        default:
            break;
    }
}

static void checkModeButtonLayout_SidebarWidget_(iSidebarWidget *d) {
    if (!d->modeButtons[0]) return;
    if (deviceType_App() == phone_AppDeviceType) {
        /* Change font size depending on orientation. */
        const int fonts[2] = {
            isPortrait_App() ? uiLabelBig_FontId : uiContent_FontId,
            isPortrait_App() ? uiLabelBigBold_FontId : uiContentBold_FontId
        };
        if (d->itemFonts[0] != fonts[0]) {
            d->itemFonts[0] = fonts[0];
            d->itemFonts[1] = fonts[1];
//            updateMetrics_SidebarWidget_(d);
            updateItemHeight_SidebarWidget_(d);
        }
        setButtonFont_SidebarWidget(d, isPortrait_App() ? uiLabelBig_FontId : uiLabel_FontId);
    }
    const iBool isTight =
        (width_Rect(bounds_Widget(as_Widget(d->modeButtons[0]))) < d->maxButtonLabelWidth);
    for (int i = 0; i < max_SidebarMode; i++) {
        iLabelWidget *button = d->modeButtons[i];
        if (!button) continue;
        setAlignVisually_LabelWidget(button, isTight);
        setFlags_Widget(as_Widget(button), tight_WidgetFlag, isTight);
        if (i == feeds_SidebarMode && d->numUnreadEntries) {
            updateText_LabelWidget(
                button,
                collectNewFormat_String("%s " uiTextAction_ColorEscape "%zu%s%s",
                                        tightModeLabels_[i],
                                        d->numUnreadEntries,
                                        !isTight ? " " : "",
                                        !isTight
                                            ? formatCStrs_Lang("sidebar.unread.n", d->numUnreadEntries)
                                            : ""));
        }
        else {
            updateTextCStr_LabelWidget(button,
                                       isTight ? tightModeLabels_[i] : normalModeLabels_[i]);
        }
    }
}

void setWidth_SidebarWidget(iSidebarWidget *d, float widthAsGaps) {
    iWidget *w = as_Widget(d);
    const iBool isFixedWidth = deviceType_App() == phone_AppDeviceType;
    int width = widthAsGaps * gap_UI; /* in pixels */
    if (!isFixedWidth) {
        /* Even less space if the other sidebar is visible, too. */
        const iWidget *other = findWidget_App(d->side == left_SidebarSide ? "sidebar2" : "sidebar");
        const int otherWidth = isVisible_Widget(other) ? width_Widget(other) : 0;
        width = iClamp(width, 30 * gap_UI, size_Root(w->root).x - 50 * gap_UI - otherWidth);
    }
    d->widthAsGaps = (float) width / (float) gap_UI;
    w->rect.size.x = width;
    arrange_Widget(findWidget_Root("stack"));
    checkModeButtonLayout_SidebarWidget_(d);
    updateItemHeight_SidebarWidget_(d);
}

iBool handleBookmarkEditorCommands_SidebarWidget_(iWidget *editor, const char *cmd) {
    if (equal_Command(cmd, "dlg.bookmark.setfolder")) {
        setBookmarkEditorFolder_Widget(editor, arg_Command(cmd));
        return iTrue;
    }
    if (equal_Command(cmd, "bmed.accept") || equal_Command(cmd, "bmed.cancel")) {
        iAssert(startsWith_String(id_Widget(editor), "bmed."));
        iSidebarWidget *d = findWidget_App(cstr_String(id_Widget(editor)) + 5); /* bmed.sidebar */
        if (equal_Command(cmd, "bmed.accept")) {
            const iString *title = text_InputWidget(findChild_Widget(editor, "bmed.title"));
            const iString *url   = text_InputWidget(findChild_Widget(editor, "bmed.url"));
            const iString *tags  = text_InputWidget(findChild_Widget(editor, "bmed.tags"));
            const iString *icon  = collect_String(trimmed_String(
                                        text_InputWidget(findChild_Widget(editor, "bmed.icon"))));
            const iSidebarItem *item = d->contextItem;
            iAssert(item); /* hover item cannot have been changed */
            iBookmark *bm = get_Bookmarks(bookmarks_App(), item->id);
            set_String(&bm->title, title);
            if (!isFolder_Bookmark(bm)) {
                set_String(&bm->url, url);
                set_String(&bm->tags, tags);
                if (isEmpty_String(icon)) {
                    removeTag_Bookmark(bm, userIcon_BookmarkTag);
                    bm->icon = 0;
                }
                else {
                    addTagIfMissing_Bookmark(bm, userIcon_BookmarkTag);
                    bm->icon = first_String(icon);
                }
                addOrRemoveTag_Bookmark(bm, homepage_BookmarkTag,
                                        isSelected_Widget(findChild_Widget(editor, "bmed.tag.home")));
                addOrRemoveTag_Bookmark(bm, remoteSource_BookmarkTag,
                                        isSelected_Widget(findChild_Widget(editor, "bmed.tag.remote")));
                addOrRemoveTag_Bookmark(bm, linkSplit_BookmarkTag,
                                        isSelected_Widget(findChild_Widget(editor, "bmed.tag.linksplit")));
            }
            const iBookmark *folder = userData_Object(findChild_Widget(editor, "bmed.folder"));
            if (!folder || !hasParent_Bookmark(folder, id_Bookmark(bm))) {
                bm->parentId = folder ? id_Bookmark(folder) : 0;
            }
            postCommand_App("bookmarks.changed");
        }
        setupSheetTransition_Mobile(editor, iFalse);
        destroy_Widget(editor);
        return iTrue;
    }
    return iFalse;
}

static iBool handleSidebarCommand_SidebarWidget_(iSidebarWidget *d, const char *cmd) {
    iWidget *w = as_Widget(d);
    if (equal_Command(cmd, "width")) {
        setWidth_SidebarWidget(d, arg_Command(cmd) *
                               (argLabel_Command(cmd, "gaps") ? 1.0f : (1.0f / gap_UI)));
        return iTrue;
    }
    else if (equal_Command(cmd, "mode")) {
        const iBool wasChanged = setMode_SidebarWidget(d, arg_Command(cmd));
        updateItems_SidebarWidget_(d);
        if ((argLabel_Command(cmd, "show") && !isVisible_Widget(w)) ||
            (argLabel_Command(cmd, "toggle") && (!isVisible_Widget(w) || !wasChanged))) {
            postCommandf_App("%s.toggle", cstr_String(id_Widget(w)));
        }
        scrollOffset_ListWidget(d->list, 0);
        if (wasChanged) {
            postCommandf_App("%s.mode.changed arg:%d", cstr_String(id_Widget(w)), d->mode);
        }
        refresh_Widget(findChild_Widget(w, "buttons"));
        return iTrue;
    }
    else if (equal_Command(cmd, "toggle")) {
        if (arg_Command(cmd) && isVisible_Widget(w)) {
            return iTrue;
        }
        const iBool isAnimated = prefs_App()->uiAnimations &&
                                 argLabel_Command(cmd, "noanim") == 0 &&
                                 (d->side == left_SidebarSide || deviceType_App() != phone_AppDeviceType);
        int visX = 0;
        if (isVisible_Widget(w)) {
            visX = left_Rect(bounds_Widget(w)) - left_Rect(w->root->widget->rect);
        }
        setFlags_Widget(w, hidden_WidgetFlag, isVisible_Widget(w));
        /* Safe area inset for mobile. */
        const int safePad = (d->side == left_SidebarSide ? left_Rect(safeRect_Root(w->root)) : 0);
        if (isVisible_Widget(w)) {
            setFlags_Widget(w, keepOnTop_WidgetFlag, iFalse);
            w->rect.size.x = d->widthAsGaps * gap_UI;
            invalidate_ListWidget(d->list);
            if (isAnimated) {
                setFlags_Widget(w, horizontalOffset_WidgetFlag, iTrue);
                setVisualOffset_Widget(
                    w, (d->side == left_SidebarSide ? -1 : 1) * (w->rect.size.x + safePad), 0, 0);
                setVisualOffset_Widget(w, 0, 300, easeOut_AnimFlag | softer_AnimFlag);
            }
        }
        else if (isAnimated) {
            setFlags_Widget(w, horizontalOffset_WidgetFlag, iTrue);
            if (d->side == right_SidebarSide) {
                setVisualOffset_Widget(w, visX, 0, 0);
                setVisualOffset_Widget(
                    w, visX + w->rect.size.x + safePad, 300, easeOut_AnimFlag | softer_AnimFlag);
            }
            else {
                setFlags_Widget(w, keepOnTop_WidgetFlag, iTrue);
                setVisualOffset_Widget(
                    w, -w->rect.size.x - safePad, 300, easeOut_AnimFlag | softer_AnimFlag);
            }
        }
        updateToolbarColors_Root(w->root);
        arrange_Widget(w->parent);
        /* BUG: Rearranging because the arrange above didn't fully resolve the height. */
        arrange_Widget(w);
        updateSize_DocumentWidget(document_App());
        if (isVisible_Widget(w)) {
            updateItems_SidebarWidget_(d);
            scrollOffset_ListWidget(d->list, 0);
        }
        refresh_Widget(w->parent);
        return iTrue;
    }
    return iFalse;
}

static void bookmarkMoved_SidebarWidget_(iSidebarWidget *d, size_t index, size_t dstIndex,
                                         iBool isBefore) {
    const iSidebarItem *movingItem = item_ListWidget(d->list, index);
    const iBool         isLast     = (dstIndex == numItems_ListWidget(d->list));
    const iSidebarItem *dstItem    = item_ListWidget(d->list,
                                                     isLast ? numItems_ListWidget(d->list) - 1
                                                            : dstIndex);
    if (isLast && isBefore) isBefore = iFalse;
    const iBookmark *dst = get_Bookmarks(bookmarks_App(), dstItem->id);
    if (hasParent_Bookmark(dst, movingItem->id) || hasTag_Bookmark(dst, remote_BookmarkTag)) {
        /* Can't move a folder inside itself, and remote bookmarks cannot be reordered. */
        return;
    }
    reorder_Bookmarks(bookmarks_App(), movingItem->id, dst->order + (isBefore ? 0 : 1));
    get_Bookmarks(bookmarks_App(), movingItem->id)->parentId = dst->parentId;
    updateItems_SidebarWidget_(d);
    /* Don't confuse the user: keep the dragged item in hover state. */
    setHoverItem_ListWidget(d->list, dstIndex + (isBefore ? 0 : 1) + (index < dstIndex ? -1 : 0));
    postCommandf_App("bookmarks.changed nosidebar:%p", d); /* skip this sidebar since we updated already */
}

static void bookmarkMovedOntoFolder_SidebarWidget_(iSidebarWidget *d, size_t index,
                                                   size_t folderIndex) {
    const iSidebarItem *movingItem = item_ListWidget(d->list, index);
    const iSidebarItem *dstItem    = item_ListWidget(d->list, folderIndex);
    iBookmark *bm = get_Bookmarks(bookmarks_App(), movingItem->id);
    bm->parentId = dstItem->id;
    postCommand_App("bookmarks.changed");
}

static size_t numBookmarks_(const iPtrArray *bmList) {
    size_t num = 0;
    iConstForEach(PtrArray, i, bmList) {
        if (!isFolder_Bookmark(i.ptr) && !hasTag_Bookmark(i.ptr, remote_BookmarkTag)) {
            num++;
        }
    }
    return num;
}

static iBool processEvent_SidebarWidget_(iSidebarWidget *d, const SDL_Event *ev) {
    iWidget *w = as_Widget(d);
    /* Handle commands. */
    if (isResize_UserEvent(ev)) {
        checkModeButtonLayout_SidebarWidget_(d);
        if (deviceType_App() == phone_AppDeviceType && d->side == left_SidebarSide) {
            setFlags_Widget(w, rightEdgeDraggable_WidgetFlag, isPortrait_App());
            /* In landscape, visibility of the toolbar is controlled separately. */
            if (isVisible_Widget(w)) {
                postCommand_Widget(w, "sidebar.toggle");
            }
            setFlags_Widget(findChild_Widget(w, "buttons"),
                            drawBackgroundToHorizontalSafeArea_WidgetFlag,
                            isLandscape_App());
            setFlags_Widget(findChild_Widget(w, "actions"),
                            drawBackgroundToHorizontalSafeArea_WidgetFlag,
                            isLandscape_App());
            setFlags_Widget(as_Widget(d->list),
                            drawBackgroundToHorizontalSafeArea_WidgetFlag,
                            isLandscape_App());
            return iFalse;
        }
    }
    else if (isMetricsChange_UserEvent(ev)) {
        if (isVisible_Widget(w)) {
            w->rect.size.x = d->widthAsGaps * gap_UI;
        }
        updateMetrics_SidebarWidget_(d);
        arrange_Widget(w);
        checkModeButtonLayout_SidebarWidget_(d);
    }
    else if (ev->type == SDL_USEREVENT && ev->user.code == command_UserEventCode) {
        const char *cmd = command_UserEvent(ev);
        if (equal_Command(cmd, "tabs.changed") || equal_Command(cmd, "document.changed")) {
            updateItems_SidebarWidget_(d);
            scrollOffset_ListWidget(d->list, 0);
        }
        else if (equal_Command(cmd, "sidebar.update")) {
            d->numUnreadEntries = numUnread_Feeds();
            checkModeButtonLayout_SidebarWidget_(d);
            updateItems_SidebarWidget_(d);
        }
        else if (equal_Command(cmd, "visited.changed")) {
            d->numUnreadEntries = numUnread_Feeds();
            checkModeButtonLayout_SidebarWidget_(d);
            if (d->mode == history_SidebarMode || d->mode == feeds_SidebarMode) {
                updateItems_SidebarWidget_(d);
            }
        }
        else if (equal_Command(cmd, "bookmarks.changed") && (d->mode == bookmarks_SidebarMode ||
                                                             d->mode == feeds_SidebarMode)) {
            if (pointerLabel_Command(cmd, "nosidebar") != d) {
                updateItems_SidebarWidget_(d);
                if (hasLabel_Command(cmd, "added")) {
                    const size_t addedId    = argLabel_Command(cmd, "added");
                    const size_t addedIndex = findItem_SidebarWidget_(d, addedId);
                    scrollToItem_ListWidget(d->list, addedIndex, 200);
                }
            }
        }
        else if (equal_Command(cmd, "idents.changed") && d->mode == identities_SidebarMode) {
            updateItems_SidebarWidget_(d);
        }
        else if (deviceType_App() == tablet_AppDeviceType && equal_Command(cmd, "toolbar.showident")) {
            postCommandf_App("sidebar.mode arg:%d toggle:1", identities_SidebarMode);
            return iTrue;
        }
        else if (isPortraitPhone_App() && isVisible_Widget(w) && d->side == left_SidebarSide &&
                 equal_Command(cmd, "swipe.forward")) {
            postCommand_App("sidebar.toggle");
            return iTrue;
        }
        else if (startsWith_CStr(cmd, cstr_String(&d->cmdPrefix))) {
            if (handleSidebarCommand_SidebarWidget_(d, cmd + size_String(&d->cmdPrefix))) {
                return iTrue;
            }
        }
        else if (isCommand_Widget(w, ev, "mouse.clicked")) {
            if (argLabel_Command(cmd, "button") == SDL_BUTTON_LEFT) {
                if (arg_Command(cmd)) {
                    setFlags_Widget(d->resizer, pressed_WidgetFlag, iTrue);
                    setBackgroundColor_Widget(d->resizer, uiBackgroundFramelessHover_ColorId);
                    setMouseGrab_Widget(d->resizer);
                    refresh_Widget(d->resizer);
                }
                else {
                    setFlags_Widget(d->resizer, pressed_WidgetFlag, iFalse);
                    setBackgroundColor_Widget(d->resizer, none_ColorId);
                    setMouseGrab_Widget(NULL);
                    /* Final size update in case it was resized. */
                    updateSize_DocumentWidget(document_App());
                    refresh_Widget(d->resizer);
                }
            }
            return iTrue;
        }
        else if (isCommand_Widget(w, ev, "mouse.moved")) {
            if (isResizing_SidebarWidget_(d)) {
                const iInt2 inner = windowToInner_Widget(w, coord_Command(cmd));
                const int resMid = d->resizer->rect.size.x / 2;
                setWidth_SidebarWidget(
                    d,
                    ((d->side == left_SidebarSide
                         ? inner.x
                          : (right_Rect(rect_Root(w->root)) - coord_Command(cmd).x)) +
                     resMid) / (float) gap_UI);
            }
            return iTrue;
        }
        else if (isCommand_Widget(w, ev, "list.clicked")) {
            itemClicked_SidebarWidget_(
                d, pointerLabel_Command(cmd, "item"), argU32Label_Command(cmd, "arg"));
            return iTrue;
        }
        else if (isCommand_Widget(w, ev, "list.dragged")) {
            iAssert(d->mode == bookmarks_SidebarMode);
            if (hasLabel_Command(cmd, "onto")) {
                /* Dragged onto a folder. */
                bookmarkMovedOntoFolder_SidebarWidget_(d,
                                                       argU32Label_Command(cmd, "arg"),
                                                       argU32Label_Command(cmd, "onto"));
            }
            else {
                const iBool isBefore = hasLabel_Command(cmd, "before");
                bookmarkMoved_SidebarWidget_(d,
                                             argU32Label_Command(cmd, "arg"),
                                             argU32Label_Command(cmd, isBefore ? "before" : "after"),
                                             isBefore);
            }
            return iTrue;
        }
        else if (isCommand_Widget(w, ev, "menu.closed")) {
         //   invalidateItem_ListWidget(d->list, d->contextIndex);
        }
        else if (isCommand_Widget(w, ev, "bookmark.open")) {
            const iSidebarItem *item = d->contextItem;
            if (d->mode == bookmarks_SidebarMode && item) {
                postCommandf_App("open newtab:%d url:%s",
                                 argLabel_Command(cmd, "newtab"),
                                 cstr_String(&item->url));
            }
            return iTrue;
        }
        else if (isCommand_Widget(w, ev, "bookmark.copy")) {
            const iSidebarItem *item = d->contextItem;
            if (d->mode == bookmarks_SidebarMode && item) {
                SDL_SetClipboardText(cstr_String(canonicalUrl_String(&item->url)));
            }
            return iTrue;
        }
        else if (isCommand_Widget(w, ev, "bookmark.edit")) {
            const iSidebarItem *item = d->contextItem;
            if (d->mode == bookmarks_SidebarMode && item) {
                iWidget *dlg = makeBookmarkEditor_Widget();
                setId_Widget(dlg, format_CStr("bmed.%s", cstr_String(id_Widget(w))));
                iBookmark *bm = get_Bookmarks(bookmarks_App(), item->id);
                setText_InputWidget(findChild_Widget(dlg, "bmed.title"), &bm->title);
                iInputWidget *urlInput        = findChild_Widget(dlg, "bmed.url");
                iInputWidget *tagsInput       = findChild_Widget(dlg, "bmed.tags");
                iInputWidget *iconInput       = findChild_Widget(dlg, "bmed.icon");
                iWidget *     homeTag         = findChild_Widget(dlg, "bmed.tag.home");
                iWidget *     remoteSourceTag = findChild_Widget(dlg, "bmed.tag.remote");
                iWidget *     linkSplitTag    = findChild_Widget(dlg, "bmed.tag.linksplit");
                if (!isFolder_Bookmark(bm)) {
                    setText_InputWidget(urlInput, &bm->url);
                    setText_InputWidget(tagsInput, &bm->tags);
                    if (hasTag_Bookmark(bm, userIcon_BookmarkTag)) {
                        setText_InputWidget(iconInput,
                                            collect_String(newUnicodeN_String(&bm->icon, 1)));
                    }
                    setToggle_Widget(homeTag, hasTag_Bookmark(bm, homepage_BookmarkTag));
                    setToggle_Widget(remoteSourceTag, hasTag_Bookmark(bm, remoteSource_BookmarkTag));
                    setToggle_Widget(linkSplitTag, hasTag_Bookmark(bm, linkSplit_BookmarkTag));
                }
                else {
                    setFlags_Widget(findChild_Widget(dlg, "bmed.special"),
                                    hidden_WidgetFlag | disabled_WidgetFlag,
                                    iTrue);
                    iAnyObject *notNeeded[] = { urlInput, tagsInput, iconInput, NULL };
                    iForIndices(i, notNeeded) {
                        setFlags_Widget(notNeeded[i], disabled_WidgetFlag, iTrue);
                    }
                }
                setBookmarkEditorFolder_Widget(dlg, bm ? bm->parentId : 0);
                setCommandHandler_Widget(dlg, handleBookmarkEditorCommands_SidebarWidget_);
                setFocus_Widget(findChild_Widget(dlg, "bmed.title"));
            }
            return iTrue;
        }
        else if (isCommand_Widget(w, ev, "bookmark.dup")) {
            const iSidebarItem *item = d->contextItem;
            if (d->mode == bookmarks_SidebarMode && item) {
                iBookmark *bm = get_Bookmarks(bookmarks_App(), item->id);
                const iBool isRemote = hasTag_Bookmark(bm, remote_BookmarkTag);
                iChar icon = isRemote ? 0x1f588 : bm->icon;
                iWidget *dlg = makeBookmarkCreation_Widget(&bm->url, &bm->title, icon);
                setId_Widget(dlg, format_CStr("bmed.%s", cstr_String(id_Widget(w))));
                if (!isRemote) {
                    setText_InputWidget(findChild_Widget(dlg, "bmed.tags"), &bm->tags);
                }
                setFocus_Widget(findChild_Widget(dlg, "bmed.title"));
            }
            return iTrue;
        }
        else if (isCommand_Widget(w, ev, "bookmark.tag")) {
            const iSidebarItem *item = d->contextItem;
            if (d->mode == bookmarks_SidebarMode && item) {
                const char *tag = cstr_String(string_Command(cmd, "tag"));
                iBookmark *bm = get_Bookmarks(bookmarks_App(), item->id);
                if (hasTag_Bookmark(bm, tag)) {
                    removeTag_Bookmark(bm, tag);
                    if (!iCmpStr(tag, subscribed_BookmarkTag)) {
                        removeEntries_Feeds(item->id);
                    }
                }
                else {
                    addTag_Bookmark(bm, tag);
                }
                postCommand_App("bookmarks.changed");
            }
            return iTrue;
        }
        else if (isCommand_Widget(w, ev, "bookmark.delete")) {
            const iSidebarItem *item = d->contextItem;
            if (d->mode == bookmarks_SidebarMode && item) {
                iBookmark *bm = get_Bookmarks(bookmarks_App(), item->id);
                if (isFolder_Bookmark(bm)) {
                    const iPtrArray *list = list_Bookmarks(bookmarks_App(), NULL,
                                                           filterInsideFolder_Bookmark, bm);
                    /* Folder deletion requires confirmation because folders can contain
                       any number of bookmarks and other folders. */
                    if (argLabel_Command(cmd, "confirmed") || isEmpty_PtrArray(list)) {
                        iConstForEach(PtrArray, i, list) {
                            removeEntries_Feeds(id_Bookmark(i.ptr));
                        }
                        remove_Bookmarks(bookmarks_App(), item->id);
                        postCommand_App("bookmarks.changed");
                    }
                    else {
                        const size_t numBookmarks = numBookmarks_(list);
                        makeQuestion_Widget(uiHeading_ColorEscape "${heading.confirm.bookmarks.delete}",
                                            formatCStrs_Lang("dlg.confirm.bookmarks.delete.n", numBookmarks),
                                            (iMenuItem[]){
                            { "${cancel}" },
                            { format_CStr(uiTextCaution_ColorEscape "%s",
                                          formatCStrs_Lang("dlg.bookmarks.delete.n", numBookmarks)),
                                          0, 0, format_CStr("!bookmark.delete confirmed:1 ptr:%p", d) },
                        }, 2);
                    }
                }
                else {
                    /* TODO: Move it to a Trash folder? */
                    if (remove_Bookmarks(bookmarks_App(), item->id)) {
                        removeEntries_Feeds(item->id);
                        postCommand_App("bookmarks.changed");
                    }
                }
            }
            return iTrue;
        }
        else if (isCommand_Widget(w, ev, "bookmark.addfolder")) {
            const iSidebarItem *item = d->contextItem;
            if (d->mode == bookmarks_SidebarMode) {
                postCommandf_App("bookmarks.addfolder parent:%zu",
                                 !item ? 0
                                 : item->listItem.isDropTarget
                                     ? item->id
                                     : get_Bookmarks(bookmarks_App(), item->id)->parentId);
            }
            return iTrue;
        }
        else if (isCommand_Widget(w, ev, "bookmark.sortfolder")) {
            const iSidebarItem *item = d->contextItem;
            if (d->mode == bookmarks_SidebarMode && item) {
                postCommandf_App("bookmarks.sort arg:%zu",
                                 item->listItem.isDropTarget
                                     ? item->id
                                     : get_Bookmarks(bookmarks_App(), item->id)->parentId);
            }
            return iTrue;
        }
        else if (equal_Command(cmd, "feeds.update.finished")) {
            d->numUnreadEntries = argLabel_Command(cmd, "unread");
            checkModeButtonLayout_SidebarWidget_(d);
            if (d->mode == feeds_SidebarMode) {
                updateItems_SidebarWidget_(d);
            }
        }
        else if (equalWidget_Command(cmd, w, "feeds.mode")) {
            d->feedsMode = arg_Command(cmd);
            updateItems_SidebarWidget_(d);
            return iTrue;
        }
        else if (equal_Command(cmd, "feeds.markallread") && d->mode == feeds_SidebarMode) {
            iConstForEach(PtrArray, i, listEntries_Feeds()) {
                const iFeedEntry *entry = i.ptr;
                const iString *url = url_FeedEntry(entry);
                if (!containsUrl_Visited(visited_App(), url)) {
                    visitUrl_Visited(visited_App(), url, transient_VisitedUrlFlag);
                }
            }
            postCommand_App("visited.changed");
            return iTrue;
        }
        else if (startsWith_CStr(cmd, "feed.entry.") && d->mode == feeds_SidebarMode) {
            const iSidebarItem *item = d->contextItem;
            if (item) {
                if (isCommand_Widget(w, ev, "feed.entry.opentab")) {
                    postCommandString_Root(get_Root(), feedEntryOpenCommand_String(&item->url, 1));
                    return iTrue;
                }
                if (isCommand_Widget(w, ev, "feed.entry.toggleread")) {
                    iVisited *vis = visited_App();
                    const iString *url = urlFragmentStripped_String(&item->url);
                    if (containsUrl_Visited(vis, url)) {
                        removeUrl_Visited(vis, url);
                    }
                    else {
                        visitUrl_Visited(vis, url, transient_VisitedUrlFlag | kept_VisitedUrlFlag);
                    }
                    postCommand_App("visited.changed");
                    return iTrue;
                }
                if (isCommand_Widget(w, ev, "feed.entry.bookmark")) {
                    makeBookmarkCreation_Widget(&item->url, &item->label, item->icon);
                    if (deviceType_App() == desktop_AppDeviceType) {
                        postCommand_App("focus.set id:bmed.title");
                    }
                    return iTrue;
                }
                iBookmark *feedBookmark = get_Bookmarks(bookmarks_App(), item->id);
                if (feedBookmark) {
                    if (isCommand_Widget(w, ev, "feed.entry.openfeed")) {
                        postCommandf_App("open url:%s", cstr_String(&feedBookmark->url));
                        return iTrue;
                    }
                    if (isCommand_Widget(w, ev, "feed.entry.edit")) {
                        makeFeedSettings_Widget(id_Bookmark(feedBookmark));
                        return iTrue;
                    }
                    if (isCommand_Widget(w, ev, "feed.entry.unsubscribe")) {
                        if (arg_Command(cmd)) {
                            removeTag_Bookmark(feedBookmark, subscribed_BookmarkTag);
                            removeEntries_Feeds(id_Bookmark(feedBookmark));
                            updateItems_SidebarWidget_(d);
                        }
                        else {
                            makeQuestion_Widget(
                                uiTextCaution_ColorEscape "${heading.unsub}",
                                format_CStr(cstr_Lang("dlg.confirm.unsub"),
                                            cstr_String(&feedBookmark->title)),
                                (iMenuItem[]){
                                    { "${cancel}", 0, 0, NULL },
                                    { uiTextCaution_ColorEscape "${dlg.unsub}",
                                      0,
                                      0,
                                      format_CStr("!feed.entry.unsubscribe arg:1 ptr:%p", d) } },
                                2);
                        }
                        return iTrue;
                    }
                }
            }
        }
        else if (isCommand_Widget(w, ev, "ident.use")) {
            iGmIdentity *  ident  = menuIdentity_SidebarWidget_(d);
            const iString *tabUrl = url_DocumentWidget(document_App());
            if (ident) {
                if (argLabel_Command(cmd, "clear")) {
                    clearUse_GmIdentity(ident);
                }
                else if (arg_Command(cmd)) {
                    signIn_GmCerts(certs_App(), ident, tabUrl);
                    postCommand_App("navigate.reload");
                }
                else {
                    signOut_GmCerts(certs_App(), tabUrl);
                    postCommand_App("navigate.reload");
                }
                saveIdentities_GmCerts(certs_App());
                updateItems_SidebarWidget_(d);
            }
            return iTrue;
        }
        else if (isCommand_Widget(w, ev, "ident.edit")) {
            const iGmIdentity *ident = menuIdentity_SidebarWidget_(d);
            if (ident) {
                makeValueInput_Widget(get_Root()->widget,
                                      &ident->notes,
                                      uiHeading_ColorEscape "${heading.ident.notes}",
                                      format_CStr(cstr_Lang("dlg.ident.notes"), cstr_String(name_GmIdentity(ident))),
                                      uiTextAction_ColorEscape "${dlg.default}",
                                      format_CStr("!ident.setnotes ident:%p ptr:%p", ident, d));
            }
            return iTrue;
        }
        else if (isCommand_Widget(w, ev, "ident.fingerprint")) {
            const iGmIdentity *ident = menuIdentity_SidebarWidget_(d);
            if (ident) {
                const iString *fps = collect_String(
                    hexEncode_Block(collect_Block(fingerprint_TlsCertificate(ident->cert))));
                SDL_SetClipboardText(cstr_String(fps));
            }
            return iTrue;
        }
        else if (isCommand_Widget(w, ev, "ident.export")) {
            const iGmIdentity *ident = menuIdentity_SidebarWidget_(d);
            if (ident) {
                iString *pem = collect_String(pem_TlsCertificate(ident->cert));
                append_String(pem, collect_String(privateKeyPem_TlsCertificate(ident->cert)));
                iDocumentWidget *expTab = newTab_App(NULL, iTrue);
                setUrlAndSource_DocumentWidget(
                    expTab,
                    collectNewFormat_String("file:%s.pem", cstr_String(name_GmIdentity(ident))),
                    collectNewCStr_String("text/plain"),
                    utf8_String(pem));
            }
            return iTrue;
        }
        else if (isCommand_Widget(w, ev, "ident.setnotes")) {
            iGmIdentity *ident = pointerLabel_Command(cmd, "ident");
            if (ident) {
                setCStr_String(&ident->notes, suffixPtr_Command(cmd, "value"));
                updateItems_SidebarWidget_(d);
            }
            return iTrue;
        }
        else if (isCommand_Widget(w, ev, "ident.pickicon")) {
            return iTrue;
        }
        else if (isCommand_Widget(w, ev, "ident.reveal")) {
            const iGmIdentity *ident = menuIdentity_SidebarWidget_(d);
            if (ident) {
                const iString *crtPath = certificatePath_GmCerts(certs_App(), ident);
                if (crtPath) {
                    revealPath_App(crtPath);
                }
            }
            return iTrue;
        }
        else if (isCommand_Widget(w, ev, "ident.delete")) {
            iSidebarItem *item = d->contextItem;
            if (argLabel_Command(cmd, "confirm")) {
                makeQuestion_Widget(
                    uiTextCaution_ColorEscape "${heading.ident.delete}",
                    format_CStr(cstr_Lang("dlg.confirm.ident.delete"),
                                uiTextAction_ColorEscape,
                                cstr_String(&item->label),
                                uiText_ColorEscape),
                    (iMenuItem[]){ { "${cancel}", 0, 0, NULL },
                                   { uiTextCaution_ColorEscape "${dlg.ident.delete}",
                                     0,
                                     0,
                                     format_CStr("!ident.delete confirm:0 ptr:%p", d) } },
                    2);
                return iTrue;
            }
            deleteIdentity_GmCerts(certs_App(), menuIdentity_SidebarWidget_(d));
            postCommand_App("idents.changed");
            return iTrue;
        }
        else if (isCommand_Widget(w, ev, "history.delete")) {
            if (d->contextItem && !isEmpty_String(&d->contextItem->url)) {
                removeUrl_Visited(visited_App(), &d->contextItem->url);
                updateItems_SidebarWidget_(d);
                scrollOffset_ListWidget(d->list, 0);
            }
            return iTrue;
        }
        else if (isCommand_Widget(w, ev, "history.copy")) {
            const iSidebarItem *item = d->contextItem;
            if (item && !isEmpty_String(&item->url)) {
                SDL_SetClipboardText(cstr_String(canonicalUrl_String(&item->url)));
            }
            return iTrue;
        }
        else if (isCommand_Widget(w, ev, "history.addbookmark")) {
            const iSidebarItem *item = d->contextItem;
            if (!isEmpty_String(&item->url)) {
                makeBookmarkCreation_Widget(
                    &item->url,
                    collect_String(newRange_String(urlHost_String(&item->url))),
                    0x1f310 /* globe */);
                if (deviceType_App() == desktop_AppDeviceType) {
                    postCommand_App("focus.set id:bmed.title");
                }
            }
        }
        else if (equal_Command(cmd, "history.clear")) {
            if (argLabel_Command(cmd, "confirm")) {
                makeQuestion_Widget(uiTextCaution_ColorEscape "${heading.history.clear}",
                                    "${dlg.confirm.history.clear}",
                                    (iMenuItem[]){ { "${cancel}", 0, 0, NULL },
                                                   { uiTextCaution_ColorEscape "${dlg.history.clear}",
                                                     0, 0, "history.clear confirm:0" } },
                                    2);
            }
            else {
                clear_Visited(visited_App());
                updateItems_SidebarWidget_(d);
                scrollOffset_ListWidget(d->list, 0);
            }
            return iTrue;
        }
    }
    if (ev->type == SDL_MOUSEMOTION &&
        (!isVisible_Widget(d->menu) && !isVisible_Widget(d->modeMenu))) {
        const iInt2 mouse = init_I2(ev->motion.x, ev->motion.y);
        if (contains_Widget(d->resizer, mouse)) {
            setCursor_Window(get_Window(), SDL_SYSTEM_CURSOR_SIZEWE);
        }
        /* Update cursor. */
        else if (contains_Widget(w, mouse)) {
            const iSidebarItem *item = constHoverItem_ListWidget(d->list);
            if (item && d->mode != identities_SidebarMode) {
                setCursor_Window(get_Window(),
                                 item->listItem.isSeparator ? SDL_SYSTEM_CURSOR_ARROW
                                                            : SDL_SYSTEM_CURSOR_HAND);
            }
            else {
                setCursor_Window(get_Window(), SDL_SYSTEM_CURSOR_ARROW);
            }
        }
        if (d->contextIndex != iInvalidPos) {
            invalidateItem_ListWidget(d->list, d->contextIndex);
            d->contextIndex = iInvalidPos;
        }
    }
    /* Update context menu items. */
    if ((d->menu || d->mode == identities_SidebarMode) && ev->type == SDL_MOUSEBUTTONDOWN) {
        if (ev->button.button == SDL_BUTTON_RIGHT) {
            d->contextItem = NULL;
            if (!isVisible_Widget(d->menu)) {
                updateMouseHover_ListWidget(d->list);
            }
            if (constHoverItem_ListWidget(d->list) || isVisible_Widget(d->menu)) {
                d->contextItem = hoverItem_ListWidget(d->list);
                /* Context is drawn in hover state. */
                if (d->contextIndex != iInvalidPos) {
                    invalidateItem_ListWidget(d->list, d->contextIndex);
                }
                d->contextIndex = hoverItemIndex_ListWidget(d->list);
                updateContextMenu_SidebarWidget_(d);                
                /* TODO: Some callback-based mechanism would be nice for updating menus right
                   before they open? At least move these to `updateContextMenu_ */
                if (d->mode == bookmarks_SidebarMode && d->contextItem) {
                    const iBookmark *bm = get_Bookmarks(bookmarks_App(), d->contextItem->id);
                    if (bm) {
                        setMenuItemLabel_Widget(d->menu,
                                                "bookmark.tag tag:homepage",
                                                hasTag_Bookmark(bm, homepage_BookmarkTag)
                                                    ? home_Icon " ${bookmark.untag.home}"
                                                    : home_Icon " ${bookmark.tag.home}");
                        setMenuItemLabel_Widget(d->menu,
                                                "bookmark.tag tag:subscribed",
                                                hasTag_Bookmark(bm, subscribed_BookmarkTag)
                                                    ? star_Icon " ${bookmark.untag.sub}"
                                                    : star_Icon " ${bookmark.tag.sub}");
                        setMenuItemLabel_Widget(d->menu,
                                                "bookmark.tag tag:remotesource",
                                                hasTag_Bookmark(bm, remoteSource_BookmarkTag)
                                                    ? downArrowBar_Icon " ${bookmark.untag.remote}"
                                                    : downArrowBar_Icon " ${bookmark.tag.remote}");
                    }
                }
                else if (d->mode == feeds_SidebarMode && d->contextItem) {
                    const iBool   isRead   = d->contextItem->indent == 0;
                    setMenuItemLabel_Widget(d->menu,
                                            "feed.entry.toggleread",
                                            isRead ? circle_Icon " ${feeds.entry.markunread}"
                                                   : circleWhite_Icon " ${feeds.entry.markread}");
                }
                else if (d->mode == identities_SidebarMode) {
                    const iGmIdentity *ident  = constHoverIdentity_SidebarWidget_(d);
                    const iString *    docUrl = url_DocumentWidget(document_App());
                    iForEach(ObjectList, i, children_Widget(d->menu)) {
                        if (isInstance_Object(i.object, &Class_LabelWidget)) {
                            iLabelWidget *menuItem = i.object;
                            const char *  cmdItem  = cstr_String(command_LabelWidget(menuItem));
                            if (equal_Command(cmdItem, "ident.use")) {
                                const iBool cmdUse   = arg_Command(cmdItem) != 0;
                                const iBool cmdClear = argLabel_Command(cmdItem, "clear") != 0;
                                setFlags_Widget(
                                    as_Widget(menuItem),
                                    disabled_WidgetFlag,
                                    (cmdClear && !isUsed_GmIdentity(ident)) ||
                                        (!cmdClear && cmdUse && isUsedOn_GmIdentity(ident, docUrl)) ||
                                        (!cmdClear && !cmdUse && !isUsedOn_GmIdentity(ident, docUrl)));
                            }
                        }
                    }
                }
            }
        }
    }
    if (ev->type == SDL_KEYDOWN) {
        const int key   = ev->key.keysym.sym;
        const int kmods = keyMods_Sym(ev->key.keysym.mod);
        /* Hide the sidebar when Escape is pressed. */
        if (kmods == 0 && key == SDLK_ESCAPE && isVisible_Widget(d)) {
            postCommand_Widget(d, "%s.toggle", cstr_String(id_Widget(w)));
            return iTrue;
        }
    }
    if (ev->type == SDL_MOUSEBUTTONDOWN &&
        contains_Widget(as_Widget(d->list), init_I2(ev->button.x, ev->button.y))) {
        if (hoverItem_ListWidget(d->list) || isVisible_Widget(d->menu)) {
            /* Update the menu before opening. */
            /* TODO: This kind of updating is already done above, and in `updateContextMenu_`... */
            if (d->mode == bookmarks_SidebarMode && !isVisible_Widget(d->menu)) {
                /* Remote bookmarks have limitations. */
                const iSidebarItem *hoverItem = hoverItem_ListWidget(d->list);
                iAssert(hoverItem);
                const iBookmark *  bm              = get_Bookmarks(bookmarks_App(), hoverItem->id);
                const iBool        isRemote        = hasTag_Bookmark(bm, remote_BookmarkTag);
                static const char *localOnlyCmds[] = { "bookmark.edit",
                                                       "bookmark.delete",
                                                       "bookmark.tag tag:" subscribed_BookmarkTag,
                                                       "bookmark.tag tag:" homepage_BookmarkTag,
                                                       "bookmark.tag tag:" remoteSource_BookmarkTag,
                                                       "bookmark.tag tag:" subscribed_BookmarkTag };
                iForIndices(i, localOnlyCmds) {
                    setFlags_Widget(as_Widget(findMenuItem_Widget(d->menu, localOnlyCmds[i])),
                                    disabled_WidgetFlag,
                                    isRemote);
                }
            }
            processContextMenuEvent_Widget(d->menu, ev, {});
        }
        else if (!constHoverItem_ListWidget(d->list) || isVisible_Widget(d->modeMenu)) {
            processContextMenuEvent_Widget(d->modeMenu, ev, {});
        }
    }
    return processEvent_Widget(w, ev);
}

static void draw_SidebarWidget_(const iSidebarWidget *d) {
    const iWidget *w      = constAs_Widget(d);
    const iRect    bounds = bounds_Widget(w);
    iPaint p;
    init_Paint(&p);
    if (!isPortraitPhone_App()) { /* this would erase page contents during transition on the phone */
        if (flags_Widget(w) & visualOffset_WidgetFlag &&
            flags_Widget(w) & horizontalOffset_WidgetFlag && isVisible_Widget(w)) {
            fillRect_Paint(&p, boundsWithoutVisualOffset_Widget(w), tmBackground_ColorId);
        }
    }
    draw_Widget(w);
    if (isVisible_Widget(w)) {
        drawVLine_Paint(
            &p,
            addX_I2(d->side == left_SidebarSide ? topRight_Rect(bounds) : topLeft_Rect(bounds), -1),
            height_Rect(bounds),
            uiSeparator_ColorId);
    }
}

static void draw_SidebarItem_(const iSidebarItem *d, iPaint *p, iRect itemRect,
                              const iListWidget *list) {
    const iSidebarWidget *sidebar = findParentClass_Widget(constAs_Widget(list),
                                                           &Class_SidebarWidget);
    const iBool isMenuVisible = isVisible_Widget(sidebar->menu);
    const iBool isDragging   = constDragItem_ListWidget(list) == d;
    const iBool isPressing   = isMouseDown_ListWidget(list) && !isDragging;
    const iBool isHover      =
            (!isMenuVisible &&
            isHover_Widget(constAs_Widget(list)) &&
            constHoverItem_ListWidget(list) == d) ||
            (isMenuVisible && sidebar->contextItem == d) ||
            isDragging;
    const int scrollBarWidth = scrollBarWidth_ListWidget(list);
#if defined (iPlatformApple)
    const int blankWidth     = 0;
#else
    const int blankWidth     = scrollBarWidth;
#endif
    const int itemHeight     = height_Rect(itemRect);
    const int iconColor      = isHover ? (isPressing ? uiTextPressed_ColorId : uiIconHover_ColorId)
                                       : uiIcon_ColorId;
    const int altIconColor   = isPressing ? uiTextPressed_ColorId : uiTextCaution_ColorId;
    const int font = sidebar->itemFonts[d->isBold ? 1 : 0];
    int bg         = uiBackgroundSidebar_ColorId;
    if (isHover) {
        bg = isPressing ? uiBackgroundPressed_ColorId
                        : uiBackgroundFramelessHover_ColorId;
        fillRect_Paint(p, itemRect, bg);
    }
    else if (d->listItem.isSelected &&
             (sidebar->mode == feeds_SidebarMode || sidebar->mode == identities_SidebarMode)) {
        bg = uiBackgroundUnfocusedSelection_ColorId;
        fillRect_Paint(p, itemRect, bg);
    }
    else if (sidebar->mode == bookmarks_SidebarMode) {
        if (d->indent) /* remote icon */  {
            bg = uiBackgroundFolder_ColorId;
            fillRect_Paint(p, itemRect, bg);
        }
    }
    iInt2 pos = itemRect.pos;
    if (sidebar->mode == documentOutline_SidebarMode) {
        const int fg = isHover ? (isPressing ? uiTextPressed_ColorId : uiTextFramelessHover_ColorId)
                               : (tmHeading1_ColorId + d->indent / (4 * gap_UI));
        drawRange_Text(font,
                       init_I2(pos.x + 3 * gap_UI + d->indent,
                               mid_Rect(itemRect).y - lineHeight_Text(font) / 2),
                       fg,
                       range_String(&d->label));
    }
    else if (sidebar->mode == feeds_SidebarMode) {
        const int fg = isHover ? (isPressing ? uiTextPressed_ColorId : uiTextFramelessHover_ColorId)
                               : uiText_ColorId;
        const int iconPad = 12 * gap_UI;
        if (d->listItem.isSeparator) {
            if (d != constItem_ListWidget(list, 0)) {
                drawHLine_Paint(p,
                                addY_I2(pos, 2 * gap_UI),
                                width_Rect(itemRect) - blankWidth,
                                uiSeparator_ColorId);
            }
            drawRange_Text(
                uiLabelLargeBold_FontId,
                add_I2(pos,
                       init_I2(3 * gap_UI,
                               itemHeight - lineHeight_Text(uiLabelLargeBold_FontId) - 1 * gap_UI)),
                uiIcon_ColorId,
                range_String(&d->meta));
        }
        else {
            const iBool isUnread = (d->indent != 0);
            const int titleFont = sidebar->itemFonts[isUnread ? 1 : 0];
            const int h1 = lineHeight_Text(uiLabel_FontId);
            const int h2 = lineHeight_Text(titleFont);
            iRect iconArea = { addY_I2(pos, 0), init_I2(iconPad, itemHeight) };
            /*
            if (isUnread) {
                fillRect_Paint(
                    p,
                    (iRect){ topLeft_Rect(iconArea), init_I2(gap_UI / 2, height_Rect(iconArea)) },
                    iconColor);
            }*/
            /* Icon. */ {
                /* TODO: Use the primary hue from the theme of this site. */
                iString str;
                initUnicodeN_String(&str, &d->icon, 1);
                /* TODO: Add to palette. */
                const int unreadIconColor = uiTextCaution_ColorId;
                const int readIconColor =
                    isDark_ColorTheme(colorTheme_App()) ? uiText_ColorId : uiAnnotation_ColorId;
                drawCentered_Text(uiLabelLarge_FontId,
                                  adjusted_Rect(iconArea, init_I2(gap_UI, 0), zero_I2()),
                                  iTrue,
                                  isHover && isPressing
                                      ? iconColor
                                      : isUnread ? unreadIconColor
                                      : d->listItem.isSelected ? iconColor
                                      : readIconColor,
                                  "%s",
                                  cstr_String(&str));
                deinit_String(&str);
            }
            /* Select the layout based on how the title fits. */
            int         metaFg    = isPressing ? fg : uiSubheading_ColorId;
            iInt2       titleSize = measureRange_Text(titleFont, range_String(&d->label)).bounds.size;
            const iInt2 metaSize  = measureRange_Text(uiLabel_FontId, range_String(&d->meta)).bounds.size;
            pos.x += iconPad;
            const int avail = width_Rect(itemRect) - iconPad - 3 * gap_UI;
            const int labelFg = isPressing ? fg : (isUnread ? uiTextStrong_ColorId : uiText_ColorId);
            if (titleSize.x > avail && metaSize.x < avail * 0.75f) {
                /* Must wrap the title. */
                pos.y += (itemHeight - h2 - h2) / 2;
                draw_Text(
                    uiLabel_FontId, addY_I2(pos, h2 - h1 - gap_UI / 8), metaFg, "%s \u2014 ", cstr_String(&d->meta));
                int skip  = metaSize.x + measure_Text(uiLabel_FontId, " \u2014 ").advance.x;
                iInt2 cur = addX_I2(pos, skip);
                const char *endPos;
                tryAdvance_Text(titleFont, range_String(&d->label), avail - skip, &endPos);
                drawRange_Text(titleFont,
                               cur,
                               labelFg,
                               (iRangecc){ constBegin_String(&d->label), endPos });
                if (endPos < constEnd_String(&d->label)) {
                    drawRange_Text(titleFont,
                                   addY_I2(pos, h2), labelFg,
                                   (iRangecc){ endPos, constEnd_String(&d->label) });
                }
            }
            else {
                pos.y += (itemHeight - h1 - h2) / 2;
                drawRange_Text(uiLabel_FontId, pos, metaFg, range_String(&d->meta));
                drawRange_Text(titleFont, addY_I2(pos, h1), labelFg, range_String(&d->label));
            }
        }
    }
    else if (sidebar->mode == bookmarks_SidebarMode) {
        const int fg = isHover ? (isPressing ? uiTextPressed_ColorId : uiTextFramelessHover_ColorId)
            : d->listItem.isDropTarget ? uiHeading_ColorId : uiText_ColorId;
        /* The icon. */
        iString str;
        init_String(&str);
        appendChar_String(&str, d->icon ? d->icon : 0x1f588);
        const int leftIndent = d->indent * gap_UI * 4;
        const iRect iconArea = { addX_I2(pos, gap_UI + leftIndent),
                                 init_I2(1.75f * lineHeight_Text(font), itemHeight) };
        drawCentered_Text(font,
                          iconArea,
                          iTrue,
                          isPressing                       ? iconColor
                          : d->icon == 0x2913 /* remote */ ? uiTextCaution_ColorId
                                                           : iconColor,
                          "%s",
                          cstr_String(&str));
        deinit_String(&str);
        const iInt2 textPos = addY_I2(topRight_Rect(iconArea), (itemHeight - lineHeight_Text(font)) / 2);
        drawRange_Text(font, textPos, fg, range_String(&d->label));
        const int metaFont = uiLabel_FontId;
        const int metaIconWidth = 4.5f * gap_UI;
        const iInt2 metaPos =
            init_I2(right_Rect(itemRect) -
                        length_String(&d->meta) *
                            metaIconWidth
                        - 2 * gap_UI - (blankWidth ? blankWidth - 1.5f * gap_UI : (gap_UI / 2)),
                    textPos.y);
        if (!isDragging) {
            fillRect_Paint(p,
                           init_Rect(metaPos.x,
                                     top_Rect(itemRect),
                                     right_Rect(itemRect) - metaPos.x,
                                     height_Rect(itemRect)),
                           bg);
        }
        iInt2 mpos = metaPos;
        iStringConstIterator iter;
        init_StringConstIterator(&iter, &d->meta);
        iRangecc range = { cstr_String(&d->meta), iter.pos };
        while (iter.value) {
            next_StringConstIterator(&iter);
            range.end = iter.pos;
            iRect iconArea = { mpos, init_I2(metaIconWidth, lineHeight_Text(metaFont)) };
            iRect visBounds = visualBounds_Text(metaFont, range);
            drawRange_Text(metaFont,
                           sub_I2(mid_Rect(iconArea), mid_Rect(visBounds)),
                           isHover && isPressing ? fg : uiTextCaution_ColorId,
                           range);
            mpos.x += metaIconWidth;
            range.start = range.end;            
        }        
    }
    else if (sidebar->mode == history_SidebarMode) {
        iBeginCollect();
        if (d->listItem.isSeparator) {
            if (!isEmpty_String(&d->meta)) {
                iInt2 drawPos = addY_I2(topLeft_Rect(itemRect), d->id);
                drawHLine_Paint(p,
                                addY_I2(drawPos, -gap_UI),
                                width_Rect(itemRect) - blankWidth,
                                uiSeparator_ColorId);
                drawRange_Text(
                    uiLabelLargeBold_FontId,
                    add_I2(drawPos,
                           init_I2(3 * gap_UI, (itemHeight - lineHeight_Text(uiLabelLargeBold_FontId)) / 2)),
                    uiIcon_ColorId,
                    range_String(&d->meta));
            }
        }
        else {
            const int fg = isHover ? (isPressing ? uiTextPressed_ColorId : uiTextFramelessHover_ColorId)
                                   : uiTextDim_ColorId;
            iUrl parts;
            init_Url(&parts, &d->label);
            const iBool isAbout  = equalCase_Rangecc(parts.scheme, "about");
            const iBool isGemini = equalCase_Rangecc(parts.scheme, "gemini");
            draw_Text(font,
                      add_I2(topLeft_Rect(itemRect),
                             init_I2(3 * gap_UI, (itemHeight - lineHeight_Text(font)) / 2)),
                      fg,
                      "%s%s%s%s%s%s%s%s",
                      isGemini ? "" : cstr_Rangecc(parts.scheme),
                      isGemini  ? ""
                      : isAbout ? ":"
                                : "://",
                      escape_Color(isHover ? (isPressing ? uiTextPressed_ColorId
                                                         : uiTextFramelessHover_ColorId)
                                           : uiTextStrong_ColorId),
                      cstr_Rangecc(parts.host),
                      escape_Color(fg),
                      cstr_Rangecc(parts.path),
                      !isEmpty_Range(&parts.query) ? escape_Color(isPressing ? uiTextPressed_ColorId
                                                                  : isHover  ? uiText_ColorId
                                                                             : uiAnnotation_ColorId)
                                                   : "",
                      !isEmpty_Range(&parts.query) ? cstr_Rangecc(parts.query) : "");
        }
        iEndCollect();
    }
    else if (sidebar->mode == identities_SidebarMode) {
        const int fg = isHover ? (isPressing ? uiTextPressed_ColorId : uiTextFramelessHover_ColorId)
                               : uiTextStrong_ColorId;
        const iBool isUsedOnDomain = (d->indent != 0);
        iString icon;
        initUnicodeN_String(&icon, &d->icon, 1);
        iInt2 cPos = topLeft_Rect(itemRect);
        const int indent = 1.4f * lineHeight_Text(font);
        addv_I2(&cPos,
                init_I2(3 * gap_UI,
                        (itemHeight - lineHeight_Text(uiLabel_FontId) * 2 - lineHeight_Text(font)) /
                            2));
        const int metaFg = isHover ? permanent_ColorId | (isPressing ? uiTextPressed_ColorId
                                                                     : uiTextFramelessHover_ColorId)
                                   : uiTextDim_ColorId;
        if (!d->listItem.isSelected && !isUsedOnDomain) {
            drawOutline_Text(font, cPos, metaFg, none_ColorId, range_String(&icon));
        }
        drawRange_Text(font,
                       cPos,
                       d->listItem.isSelected ? iconColor
                       : isUsedOnDomain       ? altIconColor
                                              : uiBackgroundSidebar_ColorId,
                       range_String(&icon));
        deinit_String(&icon);
        drawRange_Text(d->listItem.isSelected ? sidebar->itemFonts[1] : font,
                       add_I2(cPos, init_I2(indent, 0)),
                       fg,
                       range_String(&d->label));
        drawRange_Text(uiLabel_FontId,
                       add_I2(cPos, init_I2(indent, lineHeight_Text(font))),
                       metaFg,
                       range_String(&d->meta));
    }
}

iBeginDefineSubclass(SidebarWidget, Widget)
    .processEvent = (iAny *) processEvent_SidebarWidget_,
    .draw         = (iAny *) draw_SidebarWidget_,
iEndDefineSubclass(SidebarWidget)
