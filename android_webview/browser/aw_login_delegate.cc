// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "android_webview/browser/aw_login_delegate.h"

#include "android_webview/browser/aw_browser_context.h"
#include "base/android/jni_android.h"
#include "base/logging.h"
#include "base/supports_user_data.h"
#include "components/data_reduction_proxy/browser/data_reduction_proxy_auth_request_handler.h"
#include "components/data_reduction_proxy/browser/data_reduction_proxy_settings.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/resource_dispatcher_host.h"
#include "content/public/browser/resource_request_info.h"
#include "content/public/browser/web_contents.h"
#include "net/base/auth.h"
#include "net/url_request/url_request.h"

using namespace base::android;

using content::BrowserThread;
using content::RenderFrameHost;
using content::ResourceDispatcherHost;
using content::ResourceRequestInfo;
using content::WebContents;
using data_reduction_proxy::DataReductionProxyAuthRequestHandler;
using data_reduction_proxy::DataReductionProxySettings;

namespace {
const char* kAuthAttemptsKey = "android_webview_auth_attempts";

class UrlRequestAuthAttemptsData : public base::SupportsUserData::Data {
 public:
  UrlRequestAuthAttemptsData() : auth_attempts_(0) { }
  int auth_attempts_;
};

}  // namespace

namespace android_webview {

AwLoginDelegate::AwLoginDelegate(net::AuthChallengeInfo* auth_info,
                                 net::URLRequest* request)
    : auth_info_(auth_info),
      request_(request),
      render_process_id_(0),
      render_frame_id_(0) {
    ResourceRequestInfo::GetRenderFrameForRequest(
        request, &render_process_id_, &render_frame_id_);

    UrlRequestAuthAttemptsData* count =
        static_cast<UrlRequestAuthAttemptsData*>(
            request->GetUserData(kAuthAttemptsKey));

    if (count == NULL) {
      count = new UrlRequestAuthAttemptsData();
      request->SetUserData(kAuthAttemptsKey, count);
    }

    BrowserThread::PostTask(BrowserThread::UI, FROM_HERE,
        base::Bind(&AwLoginDelegate::HandleHttpAuthRequestOnUIThread,
                   this, (count->auth_attempts_ == 0)));
    count->auth_attempts_++;
}

AwLoginDelegate::~AwLoginDelegate() {
  // The Auth handler holds a ref count back on |this| object, so it should be
  // impossible to reach here while this object still owns an auth handler.
  DCHECK(aw_http_auth_handler_ == NULL);
}

void AwLoginDelegate::Proceed(const base::string16& user,
                              const base::string16& password) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  BrowserThread::PostTask(BrowserThread::IO, FROM_HERE,
      base::Bind(&AwLoginDelegate::ProceedOnIOThread,
                 this, user, password));
}

void AwLoginDelegate::Cancel() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  BrowserThread::PostTask(BrowserThread::IO, FROM_HERE,
      base::Bind(&AwLoginDelegate::CancelOnIOThread, this));
}

void AwLoginDelegate::HandleHttpAuthRequestOnUIThread(
    bool first_auth_attempt) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  aw_http_auth_handler_.reset(AwHttpAuthHandlerBase::Create(
      this, auth_info_.get(), first_auth_attempt));

  RenderFrameHost* render_frame_host = RenderFrameHost::FromID(
      render_process_id_, render_frame_id_);
  WebContents* web_contents = WebContents::FromRenderFrameHost(
      render_frame_host);
  AwBrowserContext* browser_context =
      AwBrowserContext::FromWebContents(web_contents);
  DataReductionProxySettings* drp_settings =
      browser_context->GetDataReductionProxySettings();
  if (drp_settings && drp_settings->IsDataReductionProxyEnabled()) {
    // The data reduction proxy auth handler should only be reset on the first
    // auth attempt, because it maintains internal state to cancel if there have
    // been too many attempts.
    if (!drp_auth_handler_.get()) {
      drp_auth_handler_.reset(new DataReductionProxyAuthRequestHandler(
          drp_settings));
    }
    DCHECK(drp_auth_handler_.get());
    base::string16 user, password;
    DataReductionProxyAuthRequestHandler::TryHandleResult drp_result =
        drp_auth_handler_->TryHandleAuthentication(
            auth_info_.get(), &user, &password);
    if (drp_result ==
            DataReductionProxyAuthRequestHandler::TRY_HANDLE_RESULT_PROCEED) {
      Proceed(user, password);
      return;
    }
    if (drp_result ==
            DataReductionProxyAuthRequestHandler::TRY_HANDLE_RESULT_CANCEL) {
      // Give up. Disable the proxy and retry.
      drp_settings->SetDataReductionProxyEnabled(false);
      Proceed(user, password);
      return;
    }
    // Fall through if |drp_result| is IGNORE
  }

  if (!aw_http_auth_handler_->HandleOnUIThread(web_contents)) {
    Cancel();
    return;
  }
}

void AwLoginDelegate::CancelOnIOThread() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  if (request_) {
    request_->CancelAuth();
    ResourceDispatcherHost::Get()->ClearLoginDelegateForRequest(request_);
    request_ = NULL;
  }
  DeleteAuthHandlerSoon();
}

void AwLoginDelegate::ProceedOnIOThread(const base::string16& user,
                                        const base::string16& password) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  if (request_) {
    request_->SetAuth(net::AuthCredentials(user, password));
    ResourceDispatcherHost::Get()->ClearLoginDelegateForRequest(request_);
    request_ = NULL;
  }
  DeleteAuthHandlerSoon();
}

void AwLoginDelegate::OnRequestCancelled() {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::IO));
  request_ = NULL;
  DeleteAuthHandlerSoon();
}

void AwLoginDelegate::DeleteAuthHandlerSoon() {
  if (!BrowserThread::CurrentlyOn(BrowserThread::UI)) {
    BrowserThread::PostTask(BrowserThread::UI, FROM_HERE,
        base::Bind(&AwLoginDelegate::DeleteAuthHandlerSoon, this));
    return;
  }
  aw_http_auth_handler_.reset();
}

}  // namespace android_webview
