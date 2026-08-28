<?php
/*
 SPDX-FileCopyrightText: © 2026 Siemens AG

 SPDX-License-Identifier: GPL-2.0-only
*/

use Fossology\Lib\Auth\Auth;
use Fossology\Lib\Dao\ThesmoHighlightBrowserDao;
use Fossology\Lib\Dao\UploadDao;
use Symfony\Component\HttpFoundation\JsonResponse;
use Symfony\Component\HttpFoundation\Response;

/**
 * @class AjaxThesmoHighlightBrowser
 * @brief Writes the three clearing texts across every file a highlight covers.
 *
 * The group is resolved through the same reader the page was rendered from,
 * so a save reaches the files it showed and no others.
 */
class AjaxThesmoHighlightBrowser extends FO_Plugin
{
  /** @var UploadDao */
  private $uploadDao;
  /** @var ThesmoHighlightBrowserDao */
  private $browserDao;

  function __construct()
  {
    $this->Name = "ajax-thesmo-highlight-browser";
    $this->Title = _("Ajax Thesmo Highlight Acknowledgement");
    $this->DBaccess = PLUGIN_DB_WRITE;
    $this->OutputType = 'JSON';
    $this->LoginFlag = 0;
    parent::__construct();
    $this->uploadDao = $GLOBALS['container']->get('dao.upload');
    $this->browserDao = $GLOBALS['container']->get('dao.thesmo_highlight_browser');
  }

  function PostInitialize()
  {
    $this->State = PLUGIN_STATE_READY;
    return $this->State;
  }

  function Output()
  {
    $action = GetParm("action", PARM_STRING);
    $uploadId = GetParm("upload", PARM_INTEGER);
    $licenseId = GetParm("rf", PARM_INTEGER);
    $len = GetParm("len", PARM_INTEGER);
    $groupId = Auth::getGroupId();

    // A length of zero is not a highlight, so empty() suits it as well as
    // the ids.
    if (empty($uploadId) || empty($licenseId) || empty($len)) {
      return new Response('Missing or invalid upload, license or length',
        Response::HTTP_BAD_REQUEST, array('Content-type' => 'text/plain'));
    }
    if (!$this->uploadDao->isAccessible($uploadId, $groupId)) {
      return new Response('Permission denied', Response::HTTP_FORBIDDEN,
        array('Content-type' => 'text/plain'));
    }
    try {
      $agentId = $this->browserDao->latestThesmoAgent($uploadId);
    } catch (\Exception $e) {
      // LatestScannerProxy throws where an ars table is absent, and a silent
      // success would read as a save that did nothing.
      return new JsonResponse(array('error' =>
        _("Could not determine the thesmo scan: ") . $e->getMessage()),
        Response::HTTP_INTERNAL_SERVER_ERROR);
    }
    if ($agentId === null) {
      return new JsonResponse(array('error' =>
        _("This upload has no thesmo results, so it has no highlights.")),
        Response::HTTP_CONFLICT);
    }

    switch ($action) {
      case "update":
        // GetParm reads the query string before the body, so without this a
        // write is reachable by GET, which any other site can issue with the
        // user's cookies. There is no CSRF token here to require instead.
        if (!isset($_SERVER['REQUEST_METHOD'])
            || strtoupper($_SERVER['REQUEST_METHOD']) !== 'POST') {
          return new Response('This action requires POST',
            Response::HTTP_METHOD_NOT_ALLOWED,
            array('Content-type' => 'text/plain'));
        }
        if (!$this->uploadDao->isEditable($uploadId, $groupId)) {
          return new Response('Permission denied', Response::HTTP_FORBIDDEN,
            array('Content-type' => 'text/plain'));
        }
        return $this->doUpdate($uploadId, $groupId, $agentId, $licenseId,
          $len);
      default:
        return new Response('Unknown action', Response::HTTP_BAD_REQUEST,
          array('Content-type' => 'text/plain'));
    }
  }

  /**
   * @brief Write one acknowledgement and comment to every file in the group.
   * @return JsonResponse
   */
  private function doUpdate($uploadId, $groupId, $agentId, $licenseId, $len)
  {
    $ack = GetParm("acknowledgement", PARM_RAW);
    $comment = GetParm("comment", PARM_RAW);
    $licenseText = GetParm("licenseText", PARM_RAW);
    $ack = $ack === null ? '' : trim($ack);
    $comment = $comment === null ? '' : trim($comment);
    $licenseText = $licenseText === null ? '' : trim($licenseText);

    $rows = $this->browserDao->getItemsInGroup($uploadId, $groupId, $agentId,
      $licenseId, $len);
    if (empty($rows)) {
      return new JsonResponse(array('files' => 0,
        'message' => _("This highlight no longer covers any file.")));
    }
    $items = array();
    foreach ($rows as $row) {
      $items[] = intval($row['uploadtree_pk']);
    }

    try {
      $written = $this->browserDao->setTextsForItems($items, $groupId,
        $licenseId, $ack, $comment, $licenseText);
    } catch (\Exception $e) {
      return new JsonResponse(array('error' => $e->getMessage()),
        Response::HTTP_INTERNAL_SERVER_ERROR);
    }
    if ($written === 0) {
      return new JsonResponse(array('error' =>
        _("Nothing was written; the group matched no editable event.")),
        Response::HTTP_CONFLICT);
    }
    return new JsonResponse(array(
      'files' => $written,
      'message' => sprintf(_("Saved to %d file(s)."), $written),
    ));
  }
}

$NewPlugin = new AjaxThesmoHighlightBrowser();
$NewPlugin->Initialize();
