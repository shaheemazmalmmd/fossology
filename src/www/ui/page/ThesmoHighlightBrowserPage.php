<?php
/*
 SPDX-FileCopyrightText: © 2026 Siemens AG

 SPDX-License-Identifier: GPL-2.0-only
*/

namespace Fossology\UI\Page;

use Fossology\Lib\Auth\Auth;
use Fossology\Lib\Dao\ThesmoHighlightBrowserDao;
use Fossology\Lib\Dao\UploadDao;
use Fossology\Lib\Plugin\DefaultPlugin;
use Symfony\Component\HttpFoundation\Request;
use Symfony\Component\HttpFoundation\Response;

/**
 * @class ThesmoHighlightBrowserPage
 * @brief Acknowledgement and comment per thesmo highlight, not per file.
 *
 * One project's headers are one text copied into every file, so this asks for
 * one acknowledgement per highlight and writes it to every file it covers.
 *
 * Scope is narrow on purpose: only licenses concluded by an agent, and only
 * where thesmo highlighted the concluded license, because the highlight is the
 * evidence being acknowledged.
 */
class ThesmoHighlightBrowserPage extends DefaultPlugin
{
  const NAME = "thesmo_highlight_browser";

  /** @var UploadDao */
  private $uploadDao;
  /** @var ThesmoHighlightBrowserDao */
  private $browserDao;

  public function __construct()
  {
    parent::__construct(self::NAME, array(
        self::TITLE => "Thesmo Highlight Browser",
        self::DEPENDENCIES => array("browse"),
        self::PERMISSION => Auth::PERM_READ,
        self::REQUIRES_LOGIN => true
    ));
    $this->uploadDao = $this->getObject('dao.upload');
    $this->browserDao = $this->getObject('dao.thesmo_highlight_browser');
  }

  /**
   * @brief Put the page beside Report Configuration, which it complements.
   */
  protected function RegisterMenus()
  {
    // Absent rather than leading somewhere empty. Menus are registered on
    // every request, so this must never be what breaks one.
    try {
      if (!$this->browserDao->thesmoRuleWasUsed(GetParm("upload",
          PARM_INTEGER))) {
        return;
      }
    } catch (\Exception $e) {
      return;
    }

    $text = _("Thesmo highlight browser");
    $menuName = "Highlight Browser";
    menu_insert("Browse-Pfile::$menuName", 6, self::NAME, $text);
    $uri = self::NAME . Traceback_parm_keep(array("upload", "item"));
    if (GetParm("mod", PARM_STRING) == self::NAME) {
      // no URI while this is the page shown, so the entry reads as current
      menu_insert("Browse::$menuName", 99);
      menu_insert("View::$menuName", 99);
      menu_insert("View-Meta::$menuName", 99);
    } else {
      menu_insert("Browse::$menuName", 99, $uri, $text);
      menu_insert("View::$menuName", 99, $uri, $text);
      menu_insert("View-Meta::$menuName", 99, $uri, $text);
    }
  }

  /**
   * @param Request $request
   * @return Response
   */
  protected function handle(Request $request)
  {
    $uploadId = intval($request->get('upload'));
    $groupId = Auth::getGroupId();

    if ($uploadId <= 0) {
      return $this->flushContent(_("No upload selected."));
    }
    if (!$this->uploadDao->isAccessible($uploadId, $groupId)) {
      return $this->flushContent(_("Permission denied for this upload."));
    }

    if ($this->uploadDao->getUpload($uploadId) === null) {
      return $this->flushContent(_("No such upload."));
    }
    if (!$this->browserDao->thesmoRuleWasUsed($uploadId)) {
      return $this->flushContent(_("This upload was not decided with the "
        . "thesmo rule, so it has no highlight-backed agent decisions."));
    }

    // Dir2Browse reads the tree, so an item from another upload would draw
    // that upload's path. Used only once it is known to belong to this one.
    $itemId = intval($request->get('item'));
    if ($itemId > 0) {
      $entry = $this->uploadDao->getUploadEntry($itemId,
        $this->uploadDao->getUploadtreeTableName($uploadId));
      if (empty($entry) || intval($entry['upload_fk']) !== $uploadId) {
        $itemId = 0;
      }
    }
    $agentId = $this->browserDao->latestThesmoAgent($uploadId);
    $vars = array(
      'uploadId' => $uploadId,
      // the folder path and the per-item menu every other browse page shows
      'micromenu' => $itemId > 0
        ? Dir2Browse(self::NAME, $itemId, null, 0, "Browse", -1, '', '',
            $this->uploadDao->getUploadtreeTableName($uploadId))
        : '',
      'editable' => $this->uploadDao->isEditable($uploadId, $groupId),
      'agentId' => $agentId,
      'groups' => $agentId === null ? array()
        : $this->browserDao->getHighlightGroups($uploadId, $groupId, $agentId),
      // cleared files the table cannot show, so the count reconciles
      'unhighlighted' => $agentId === null ? array()
        : $this->browserDao->getUnhighlighted($uploadId, $groupId, $agentId),
      'ajaxUrl' => Traceback_uri() . '?mod=ajax-thesmo-highlight-browser',
      'viewUri' => Traceback_uri() . '?mod=view-license&upload=' . $uploadId,
    );
    return $this->render('thesmo_highlight_browser.html.twig',
      $this->mergeWithDefault($vars));
  }
}

register_plugin(new ThesmoHighlightBrowserPage());
