# Copyright 2013 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

from fnmatch import fnmatch
import logging
from urlparse import urlparse

from appengine_url_fetcher import AppEngineUrlFetcher
from appengine_wrappers import IsDevServer
from caching_file_system import CachingFileSystem
from caching_rietveld_patcher import CachingRietveldPatcher
from chained_compiled_file_system import ChainedCompiledFileSystem
from compiled_file_system import  CompiledFileSystem
from host_file_system_creator import HostFileSystemCreator
from instance_servlet import InstanceServlet
from render_servlet import RenderServlet
from rietveld_patcher import RietveldPatcher, RietveldPatcherError
from object_store_creator import ObjectStoreCreator
from patched_file_system import PatchedFileSystem
from server_instance import ServerInstance
from servlet import Request, Response, Servlet
import svn_constants
import url_constants

class _PatchServletDelegate(RenderServlet.Delegate):
  def __init__(self, issue, delegate):
    self._issue = issue
    self._delegate = delegate

  def CreateServerInstanceForChannel(self, channel):
    base_object_store_creator = ObjectStoreCreator(channel,
                                                   start_empty=False)
    branch_utility = self._delegate.CreateBranchUtility(
        base_object_store_creator)
    host_file_system_creator = self._delegate.CreateHostFileSystemCreator(
        base_object_store_creator)
    # TODO(fj): Use OfflineFileSystem here once all json/idl files in api/
    # are pulled into data store by cron jobs.
    base_file_system = CachingFileSystem(
        host_file_system_creator.Create(
            branch_utility.GetChannelInfo(channel).branch),
        base_object_store_creator)
    base_compiled_fs_factory = CompiledFileSystem.Factory(
        base_file_system, base_object_store_creator)

    object_store_creator = ObjectStoreCreator('trunk@%s' % self._issue,
                                              start_empty=False)
    rietveld_patcher = CachingRietveldPatcher(
        RietveldPatcher(svn_constants.EXTENSIONS_PATH,
                        self._issue,
                        AppEngineUrlFetcher(url_constants.CODEREVIEW_SERVER)),
        object_store_creator)
    patched_file_system = PatchedFileSystem(base_file_system,
                                            rietveld_patcher)
    patched_compiled_fs_factory = CompiledFileSystem.Factory(
        patched_file_system, object_store_creator)

    compiled_fs_factory = ChainedCompiledFileSystem.Factory(
        [(patched_compiled_fs_factory, patched_file_system),
         (base_compiled_fs_factory, base_file_system)])

    return ServerInstance(channel,
                          object_store_creator,
                          patched_file_system,
                          self._delegate.CreateAppSamplesFileSystem(
                              base_object_store_creator),
                          '/_patch/%s' % self._issue,
                          compiled_fs_factory,
                          branch_utility,
                          host_file_system_creator)

class PatchServlet(Servlet):
  '''Servlet which renders patched docs.
  '''
  def __init__(self, request, delegate=None):
    self._request = request
    self._delegate = delegate or InstanceServlet.Delegate()

  def Get(self):
    if (not IsDevServer() and
        not fnmatch(urlparse(self._request.host).netloc, '*.appspot.com')):
      # Only allow patches on appspot URLs; it doesn't matter if appspot.com is
      # XSS'ed, but it matters for chrome.com.
      redirect_host = 'https://chrome-apps-doc.appspot.com'
      logging.info('Redirecting from XSS-able host %s to %s' % (
          self._request.host, redirect_host))
      return Response.Redirect(
          '%s/_patch/%s' % (redirect_host, self._request.path))

    path_with_issue = self._request.path.lstrip('/')
    if '/' in path_with_issue:
      issue, real_path = path_with_issue.split('/', 1)
    else:
      return Response.NotFound('Malformed URL. It should look like ' +
          'https://developer.chrome.com/_patch/12345/extensions/...')

    fake_path = '/trunk/%s' % real_path

    try:
      response = RenderServlet(
          Request(fake_path, self._request.host, self._request.headers),
          _PatchServletDelegate(issue, self._delegate)).Get()
      # Disable cache for patched content.
      response.headers.pop('cache-control', None)
    except RietveldPatcherError as e:
      response = Response.NotFound(e.message, {'Content-Type': 'text/plain'})

    redirect_url, permanent = response.GetRedirect()
    if redirect_url is not None:
      if redirect_url.startswith('/trunk/'):
        redirect_url = redirect_url.split('/trunk', 1)[1]
      response = Response.Redirect('/_patch/%s%s' % (issue, redirect_url),
                                   permanent)
    return response