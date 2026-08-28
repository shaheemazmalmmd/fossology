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
    return CheckARS($uploadId, $this->AgentName, "thesmo agent", "thesmo_ars");
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
