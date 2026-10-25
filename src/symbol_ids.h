#pragma once

enum {
    SYM_GETSIZE,                    // CFSFolder::_GetSize
    SYM_PREPARE,                    // CRecursiveFolderOperation::Prepare
    SYM_DO,                         // CRecursiveFolderOperation::Do
    SYM_ITEM_GETVALUE,              // CDefCollection::Item_GetValue
    SYM_GETITEMPROPERTY,            // CDefCollection::_GetItemProperty
    SYM_MAPCOLUMNTOSCID,            // CFSFolder::MapColumnToSCID
    SYM_GETDETAILSEX,               // CFSFolder::GetDetailsEx
    SYM_COMPAREIDS,                 // CFSFolder::CompareIDs
    SYM_GETFORMATFORDISPLAYFLAGS,   // CFSFolder::GetFormatForDisplayFlags
    SYM_COUNT
};

#define SYMBOL_BIT(id) (1u << (id))
#define SYMBOLS_ALL ((1u << SYM_COUNT) - 1)
