// license:GPLv3+

#pragma once

// Castcade state stream: the one place every script -> VPX call passes through.
//
// VBScript reaches the table's objects only through IDispatch::Invoke. Every class a table script can call
// derives from CastcadeDispatchImpl instead of ATL's IDispatchImpl, so that a session in the "play" role can
// replicate the script's successful property puts and a small set of methods to the watchers
// (see CastcadeStream.h). When no session is recording, the cost is one global bool test per call.

namespace castcade
{
   // True only while a session in the "play" role is recording script calls.
   extern bool g_recordCalls;

   // Called after a successful Invoke while g_recordCalls is set. Implemented in CastcadeStream.cpp.
   void OnScriptCall(IDispatch* obj, DISPID dispid, WORD flags, const DISPPARAMS* params);

   // Match role only (CASTCADE_STREAM=match), while recording: the session reads a property's value the first time
   // the script is about to put it, which is the value the table file gave it. Called before the Invoke.
   extern bool g_baselinePuts;
   void OnScriptPutBefore(IDispatch* obj, DISPID dispid, const DISPPARAMS* params);

   // Match role only, while this VPX shows another player's table: no event reaches the script (eventproxy.h),
   // the local game sleeps. One bool test per fired event otherwise. Each event kept is reported to the session
   // (OnHeldEvent), which counts them and passes a table option event on once the game is back.
   extern bool g_holdEvents;
   void OnHeldEvent(DISPID dispid, const DISPPARAMS* params);

   // Befriended by the part classes: gives CastcadeState.cpp access to their physics-driven members.
   struct StateAccess;
}

template <class T, const IID* piid, const GUID* plibid>
class CastcadeDispatchImpl : public IDispatchImpl<T, piid, plibid>
{
public:
   STDMETHOD(Invoke)(DISPID dispidMember, REFIID riid, LCID lcid, WORD wFlags, DISPPARAMS* pdispparams, VARIANT* pvarResult, EXCEPINFO* pexcepinfo, UINT* puArgErr) override
   {
      if (!castcade::g_recordCalls)
         return IDispatchImpl<T, piid, plibid>::Invoke(dispidMember, riid, lcid, wFlags, pdispparams, pvarResult, pexcepinfo, puArgErr);
      if (castcade::g_baselinePuts && (wFlags & (DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF)))
         castcade::OnScriptPutBefore(static_cast<IDispatch*>(this), dispidMember, pdispparams);
      const HRESULT hr = IDispatchImpl<T, piid, plibid>::Invoke(dispidMember, riid, lcid, wFlags, pdispparams, pvarResult, pexcepinfo, puArgErr);
      if (castcade::g_recordCalls && SUCCEEDED(hr))
         castcade::OnScriptCall(static_cast<IDispatch*>(this), dispidMember, wFlags, pdispparams);
      return hr;
   }
};
