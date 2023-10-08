
#ifndef AFV_NATIVE_API_H
#define AFV_NATIVE_API_H

#ifdef AFV_NATIVE_STATIC_DEFINE
#  define AFV_NATIVE_API
#  define AFV_NATIVE_NO_EXPORT
#else
#  ifndef AFV_NATIVE_API
#    ifdef afv_native_EXPORTS
        /* We are building this library */
#      #ifdef SFML_SYSTEM_WINDOWS
                #define AFV_NATIVE_API __declspec(dllexport)
        #else
                #define AFV_NATIVE_API __attribute__((visibility("default")))
        #endif    
#    else
        /* We are using this library */
        #ifdef SFML_SYSTEM_WINDOWS
                #define AFV_NATIVE_API __declspec(dllimport)
        #else
                #define AFV_NATIVE_API __attribute__((visibility("default")))
        #endif    
#    endif
#  endif

#  ifndef AFV_NATIVE_NO_EXPORT
        #ifdef SFML_SYSTEM_WINDOWS
                #define AFV_NATIVE_API __declspec(dllexport)
        #else
               #define AFV_NATIVE_NO_EXPORT __attribute__((visibility("hidden")))
        #endif
#endif

#ifndef AFV_NATIVE_DEPRECATED
        #ifdef SFML_SYSTEM_WINDOWS
                #define AFV_NATIVE_API __declspec(deprecated)
        #else
                #define AFV_NATIVE_DEPRECATED __attribute__ ((__deprecated__))
        #endif
#endif

#ifndef AFV_NATIVE_DEPRECATED_EXPORT
#  define AFV_NATIVE_DEPRECATED_EXPORT AFV_NATIVE_API AFV_NATIVE_DEPRECATED
#endif

#ifndef AFV_NATIVE_DEPRECATED_NO_EXPORT
#  define AFV_NATIVE_DEPRECATED_NO_EXPORT AFV_NATIVE_NO_EXPORT AFV_NATIVE_DEPRECATED
#endif

#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef AFV_NATIVE_NO_DEPRECATED
#    define AFV_NATIVE_NO_DEPRECATED
#  endif
#endif

#endif /* AFV_NATIVE_API_H */
