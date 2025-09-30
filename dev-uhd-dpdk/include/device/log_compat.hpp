#pragma once

// If you explicitly want UHD's logger, build with -DYX_USE_UHD_LOG
#if defined(YX_USE_UHD_LOG) && __has_include(<uhd/utils/log.hpp>)
  #include <uhd/utils/log.hpp>
  #define YX_LOG_INFO(tag, expr)     UHD_LOG_INFO(tag, expr)
  #define YX_LOG_WARN(tag, expr)     UHD_LOG_WARNING(tag, expr)
  #define YX_LOG_ERROR(tag, expr)    UHD_LOG_ERROR(tag, expr)
#else
  #include <iostream>
  #define YX_LOG_INFO(tag, expr)   do { std::cerr << "[" << tag << "] " << expr << "\n"; } while(0)
  #define YX_LOG_WARN(tag, expr)   do { std::cerr << "[" << tag << "][WARN] " << expr << "\n"; } while(0)
  #define YX_LOG_ERROR(tag, expr)  do { std::cerr << "[" << tag << "][ERR ] " << expr << "\n"; } while(0)
#endif

