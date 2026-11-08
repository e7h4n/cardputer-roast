#pragma once
namespace mclog {
template <typename... A> void tagInfo(A&&...) {}
template <typename... A> void tagWarn(A&&...) {}
template <typename... A> void tagError(A&&...) {}
template <typename... A> void tagDebug(A&&...) {}
template <typename... A> void info(A&&...) {}
template <typename... A> void warn(A&&...) {}
template <typename... A> void error(A&&...) {}
}  // namespace mclog
