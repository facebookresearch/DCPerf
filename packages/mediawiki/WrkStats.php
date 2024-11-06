<?hh
/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

trait WrkStats {
  abstract protected function getLogFilePath(): string;
  public function extractKeysAndValues(string $text) {
      // split text into lines
      $lines = preg_split('/\R/', $text);
      $result = array();
      foreach ($lines as $line) {
          $line = trim($line);
          // match text in the form 'key: <numeric value>'
          $matches = array();
          $match_res = preg_match('/([0-9A-Za-z ]+)\s*:\s*([0-9.]+)/', $line, &$matches);
          if ($match_res !== 1 || count($matches) < 3) {
            continue;
          }

          $key = trim($matches[1]);
          $value = trim($matches[2]);
          $value = preg_replace('/[^\d\.]/', '', $value);
          if (strpos($value, ".") !== false) {
              $value = (float)$value;
          } else {
              $value = (int)$value;
          }
          $result[$key] = $value;
      }
      return $result;
  }

  public function collectStats(): Map<string, Map<string, num>> {
    $stats = $this->extractKeysAndValues(file_get_contents($this->getLogFilePath()));
    return Map {
      'Combined' => Map {
        'Wrk requests' => (int) $stats['Transactions'],
        'Wrk wall sec' => (float) $stats['Elapsed time'],
        'Wrk RPS' => (float) $stats['Transaction rate'],
        'Wrk successful requests' => (int) $stats['Successful transactions'],
        'Wrk failed requests' => (int) $stats['Failed transactions'],
      },
    };
  }
}
