
/**
 @file   DebugDef.hpp
 @brief  デバッグ用定義
 @author DBCTRADO
*/


#ifndef LIBISDB_DEBUG_DEF_H
#define LIBISDB_DEBUG_DEF_H


#ifdef LIBISDB_DEBUG
#if defined(_MSC_VER)
#if defined(LIBISDB_DEBUG_NEW) && !defined(LIBISDB_NO_DEFINE_NEW)
#define new LIBISDB_DEBUG_NEW
#endif
#endif // _MSC_VER
#endif // LIBISDB_DEBUG


#endif // ifndef LIBISDB_DEBUG_DEF_H
