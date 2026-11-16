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

   // Befriended by the part classes: gives CastcadeState.cpp access to their physics-driven members.
   struct StateAccess;
}

template <class T, const IID* piid, const GUID* plibid>
class CastcadeDispatchImpl : public IDispatchImpl<T, piid, plibid>
{
public:
   STDMETHOD(Invoke)(DISPID dispidMember, REFIID riid, LCID lcid, WORD wFlags, DISPPARAMS* pdispparams, VARIANT* pvarResult, EXCEPINFO* pexcepinfo, UINT* puArgErr) override
   {
      const HRESULT hr = IDispatchImpl<T, piid, plibid>::Invoke(dispidMember, riid, lcid, wFlags, pdispparams, pvarResult, pexcepinfo, puArgErr);
      if (castcade::g_recordCalls && SUCCEEDED(hr))
         castcade::OnScriptCall(static_cast<IDispatch*>(this), dispidMember, wFlags, pdispparams);
      return hr;
   }
};
