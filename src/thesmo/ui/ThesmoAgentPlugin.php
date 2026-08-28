<?php
/*
 SPDX-FileCopyrightText: © 2026 Shaheem Azmal M MD

 SPDX-License-Identifier: GPL-2.0-only
*/

namespace Fossology\Thesmo\Ui;

use Fossology\Lib\Plugin\AgentPlugin;

/**
 * @class ThesmoAgentPlugin
 * @brief UI plugin for the Thesmo agent
 */
class ThesmoAgentPlugin extends AgentPlugin
{
  /** @var thesmoDesc */
  private $thesmoDesc = "Name the licence references nomos leaves unnamed. Experimental: it abstains rather than guesses, and never overrides a nomos finding.";

  public function __construct()
  {
    $this->Name = "agent_thesmo";
    $this->Title = _("Thesmo Licence Reference Analysis <img src=\"images/info_16.png\" data-toggle=\"tooltip\" title=\"" . $this->thesmoDesc . "\" class=\"info-bullet\"/>");
    $this->AgentName = "thesmo";

    parent::__construct();
  }

  /**
   * @copydoc Fossology::Lib::Plugin::AgentPlugin::AgentHasResults()
   * @see Fossology::Lib::Plugin::AgentPlugin::AgentHasResults()
   */
  function AgentHasResults($uploadId = 0)
  {
    $state = CheckARS($uploadId, $this->AgentName, "thesmo agent", "thesmo_ars");
    if ($state != 1) {
      return $state;
    }
    /* CheckARS compares agent ids, and an id for a detector is only made when
     * that detector runs. Between a model being replaced and the next scan
     * there is no id for the new one yet, so the comparison above reports the
     * stored results as current when the installed model has changed under
     * them. The digest published beside the model says otherwise, and reading
     * it is a stat rather than a pass over every artifact. */
    $installed = $this->installedModelDigest();
    if ($installed === null) {
      return $state;
    }
    $scanned = $this->scannedModelDigest($uploadId);
    if ($scanned === null || $scanned === $installed) {
      return $state;
    }
    return 2;
  }

  /**
   * @brief The model digest the installed agent would answer with.
   *
   * The agent takes its identity from the artifact bytes every time it starts
   * and this file never overrides it, so a stale one costs an unnecessary
   * rescan and can never mislabel a finding. It is written by
   * `thesmo --write-digest`, and the agent logs an error where the two
   * disagree.
   *
   * @return string|null null where no digest is published
   */
  protected function installedModelDigest()
  {
    global $SysConf;
    if (empty($SysConf['DIRECTORIES']['MODDIR'])) {
      return null;
    }
    $path = $SysConf['DIRECTORIES']['MODDIR'] . "/thesmo/model/DIGEST";
    if (! is_readable($path)) {
      return null;
    }
    $digest = trim(@file_get_contents($path));
    return preg_match('/^[0-9a-f]{16}$/', $digest) ? $digest : null;
  }

  /**
   * @brief The model digest of the last successful scan of this upload.
   *
   * The agent records its identity as agent_rev, engine and model both:
   * "4.7.1.180.abc123.m0123456789abcdef".
   *
   * @return string|null null where nothing has scanned it
   */
  protected function scannedModelDigest($uploadId)
  {
    $agentId = \LatestAgentpk($uploadId, "thesmo_ars", true);
    if (empty($agentId)) {
      return null;
    }
    $dbManager = $GLOBALS['container']->get('db.manager');
    $row = $dbManager->getSingleRow(
      "SELECT agent_rev FROM agent WHERE agent_pk = $1",
      array($agentId), __METHOD__ . ".rev");
    if ($row === false || empty($row['agent_rev'])) {
      return null;
    }
    return preg_match('/\.m([0-9a-f]{16})$/', $row['agent_rev'], $m)
      ? $m[1] : null;
  }

  /**
   * @copydoc Fossology\Lib\Plugin\AgentPlugin::AgentAdd()
   * @see \Fossology\Lib\Plugin\AgentPlugin::AgentAdd()
   */
  public function AgentAdd($jobId, $uploadId, &$errorMsg, $dependencies = [],
      $arguments = null, $request = null, $unpackArgs = null)
  {
    if ($this->AgentHasResults($uploadId) == 1) {
      return 0;
    }

    $jobQueueId = \IsAlreadyScheduled($jobId, $this->AgentName, $uploadId);
    if ($jobQueueId != 0) {
      return $jobQueueId;
    }

    // Thesmo only speaks where nomos was silent, so nomos has to have run for
    // the decider to be able to compare them.
    if (! $this->isAgentIncluded($dependencies, "agent_nomos")) {
      $dependencies[] = "agent_nomos";
    }

    return $this->doAgentAdd($jobId, $uploadId, $errorMsg, $dependencies,
        $uploadId, null, $request);
  }

  /**
   * Check if agent already included in the dependency list
   * @param mixed  $dependencies Array of job dependencies
   * @param string $agentName    Name of the agent to be checked for
   * @return boolean true if agent already in dependency list else false
   */
  protected function isAgentIncluded($dependencies, $agentName)
  {
    foreach ($dependencies as $dependency) {
      if ($dependency == $agentName) {
        return true;
      }
      if (is_array($dependency) && $agentName == $dependency['name']) {
        return true;
      }
    }
    return false;
  }
}

register_plugin(new ThesmoAgentPlugin());
