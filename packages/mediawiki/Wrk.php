<?hh
/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

final class Wrk extends Process {
  use WrkStats;

  private ?string $logfile;

  public function __construct(
    private PerfOptions $options,
    private PerfTarget $target,
    private RequestMode $mode,
    private string $time = '60',
    private string $script = 'scripts/multi-request-txt.lua',
  ) {
    parent::__construct($options->wrk);
    $this->suppress_stdout = false;

    if ($mode === RequestModes::BENCHMARK) {
      $this->logfile = tempnam($options->tempDir, 'wrk');
    } else {
      $this->logfile = tempnam($options->tempDir, 'wrk_warmup');
    }
  }

  public function start(): void {
    parent::startWorker(
      $this->logfile,
      $this->options->delayProcessLaunch,
      $this->options->traceSubProcess,
    );
  }

  <<__Override>>
  public function getExecutablePath(): string {
    if ($this->options->remoteWrk) {
      if ($this->options->noTimeLimit) {
        return 'ssh ' . $this->options->remoteWrk . ' ' .
          parent::getExecutablePath();
      }
      return 'ssh ' . $this->options->remoteWrk . ' \'timeout\'';
    }
    if ($this->options->noTimeLimit) {
      return parent::getExecutablePath();
    }
    // Wrk calls non-signal-safe functions from it's log function, which
    // it calls from signal handlers. Leads to hang.
    return 'timeout';
  }

  <<__Override>>
  protected function getArguments(): Vector<string> {
    if ($this->options->cpuBind) {
      $this->cpuRange = $this->options->helperProcessors;
    }
    $urls_file = tempnam($this->options->tempDir, 'urls');
    $urls = file_get_contents($this->target->getURLsFile());
    $urls =
      str_replace('__HTTP_PORT__', (string) PerfSettings::HttpPort(), $urls);
    // Wrk doesn't support ipv6
    $urls = str_replace('__HTTP_HOST__', 'localhost', $urls);
    $host = 'http://localhost:' . (string) PerfSettings::HttpPort();
    file_put_contents($urls_file, $urls);

    if ($this->options->remoteWrk) {
      exec('scp ' . $urls_file . ' ' .
        $this->options->remoteWrk . ':' . $this->options->wrkTmpDir);
      $urls_file = $this->options->wrkTmpDir . '/' . basename($urls_file);
    }

    $arguments = Vector {};
    if (!$this->options->noTimeLimit) {
      $arguments = Vector {
        // See Wrk::getExecutablePath()  - these arguments get passed to
        // timeout
        '--signal=9',
        $this->options->wrkTimeout,
        parent::getExecutablePath(),
      };
    }

    switch ($this->mode) {
      case RequestModes::WARMUP:
        $arguments->addAll(
          Vector {
            '-c',
            (string) PerfSettings::WarmupConcurrency(),
            '-t',
            (string) PerfSettings::WarmupConcurrency(),
            '-r',
            (string) PerfSettings::WarmupRequests(),
            '-s',
            $this->script,
            $host,
            '--',
            $urls_file,
          },
        );
        return $arguments;
      case RequestModes::WARMUP_MULTI:
        $arguments->addAll(
          Vector {
            '-c',
            $this->options->clientThreads,
            '-d',
            $this->time,
            '-s',
            $this->script,
            $host,
            '--',
            $urls_file,
          },
        );
        return $arguments;
      case RequestModes::BENCHMARK:
        if($this->options->remoteWrk) {
          $logfile = $this->options->wrkTmpDir . '/' . basename($this->logfile);
        } else {
          $logfile = $this->logfile;
        }
        $arguments->addAll(
          Vector {
            '-c',
            $this->options->clientThreads,
            '-t',
            $this->options->clientThreads,
            '-s',
            $this->script,
          },
        );

        if (!$this->options->noTimeLimit) {
          $arguments->add('-d');
          $arguments->add($this->options->wrkDuration);
        }
        $arguments->addAll(
          Vector {
            $host,
            '--',
            $urls_file,
          },
        );
        return $arguments;
      default:
        invariant_violation(
          'Unexpected request mode: %s', (string) $this->mode,
        );
    }
  }

  public function getLogFilePath(): string {
    $logfile = $this->logfile;
    invariant(
      $logfile !== null,
      'Tried to get log file path without a logfile',
    );
    return $logfile;
  }
}
