// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/extensions/api/system_info/system_info_provider.h"

#include "base/bind.h"
#include "content/public/browser/browser_thread.h"

namespace extensions {

SystemInfoProvider::SystemInfoProvider()
    : is_waiting_for_completion_(false) {
  base::SequencedWorkerPool* pool = content::BrowserThread::GetBlockingPool();
  worker_pool_ = pool->GetSequencedTaskRunnerWithShutdownBehavior(
      pool->GetSequenceToken(),
      base::SequencedWorkerPool::CONTINUE_ON_SHUTDOWN);
}

SystemInfoProvider::~SystemInfoProvider() {}

void SystemInfoProvider::PrepareQueryOnUIThread() {}

void SystemInfoProvider::InitializeProvider(const base::Closure&
    do_query_info_callback) {
  do_query_info_callback.Run();
}

void SystemInfoProvider::StartQueryInfo(
    const QueryInfoCompletionCallback& callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK(!callback.is_null());

  callbacks_.push(callback);

  if (is_waiting_for_completion_)
    return;

  is_waiting_for_completion_ = true;

  InitializeProvider(base::Bind(
      &SystemInfoProvider::StartQueryInfoPostInitialization, this));
}

void SystemInfoProvider::OnQueryCompleted(bool success) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  while (!callbacks_.empty()) {
    QueryInfoCompletionCallback callback = callbacks_.front();
    callback.Run(success);
    callbacks_.pop();
  }

  is_waiting_for_completion_ = false;
}

void SystemInfoProvider::StartQueryInfoPostInitialization() {
  PrepareQueryOnUIThread();
  // Post the custom query info task to blocking pool for information querying
  // and reply with OnQueryCompleted.
  base::PostTaskAndReplyWithResult(
      worker_pool_,
      FROM_HERE,
      base::Bind(&SystemInfoProvider::QueryInfo, this),
      base::Bind(&SystemInfoProvider::OnQueryCompleted, this));
}

}  // namespace extensions
