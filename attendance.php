<?php

header('Content-Type: text/plain; charset=UTF-8');

$csvFile = __DIR__ . '/attendance.csv';

$uid      = isset($_POST['uid']) ? trim($_POST['uid']) : '';
$name     = isset($_POST['name']) ? trim($_POST['name']) : '';
$date     = isset($_POST['date']) ? trim($_POST['date']) : '';
$timeIn   = isset($_POST['timeIn']) ? trim($_POST['timeIn']) : '';
$timeOut  = isset($_POST['timeOut']) ? trim($_POST['timeOut']) : '';
$duration = isset($_POST['duration']) ? trim($_POST['duration']) : '';


// --------------------------------------------------
// Clean UID
// --------------------------------------------------

$uid = strtoupper(preg_replace('/[^A-F0-9]/i', '', $uid));


// --------------------------------------------------
// Validate
// --------------------------------------------------

if ($uid === '' || $date === '') {
    http_response_code(400);
    echo "Missing UID or date";
    exit;
}


// --------------------------------------------------
// Create CSV if it does not exist
// --------------------------------------------------

if (!file_exists($csvFile)) {

    $fp = fopen($csvFile, 'w');

    if ($fp === false) {
        http_response_code(500);
        echo "Cannot create CSV file";
        exit;
    }

    // UTF-8 BOM for Excel
    fwrite($fp, "\xEF\xBB\xBF");

    fputcsv($fp, [
        'UID',
        'Name',
        'DateJalali',
        'TimeIn',
        'TimeOut',
        'Duration(min)'
    ]);

    fclose($fp);
}


// --------------------------------------------------
// Read existing CSV
// --------------------------------------------------

$rows = [];

$fp = fopen($csvFile, 'r');

if ($fp === false) {
    http_response_code(500);
    echo "Cannot open CSV file";
    exit;
}

while (($row = fgetcsv($fp)) !== false) {

    // Skip completely empty rows
    if (count($row) === 0 || trim(implode('', $row)) === '') {
        continue;
    }

    // Remove UTF-8 BOM from first field
    $row[0] = preg_replace('/^\xEF\xBB\xBF/', '', $row[0]);

    // Skip header
    if (strcasecmp(trim($row[0]), 'UID') === 0) {
        continue;
    }

    // Skip invalid rows
    if (trim($row[0]) === '') {
        continue;
    }

    $rows[] = $row;
}

fclose($fp);


// --------------------------------------------------
// Determine whether this is IN or OUT
// --------------------------------------------------

$isOUT = ($timeOut !== '');


// ==================================================
// OUT
// Find the latest OPEN record for this UID + date
// ==================================================

if ($isOUT) {

    $found = false;

    // Search from newest to oldest
    for ($i = count($rows) - 1; $i >= 0; $i--) {

        $rowUid  = isset($rows[$i][0]) ? strtoupper(trim($rows[$i][0])) : '';
        $rowDate = isset($rows[$i][2]) ? trim($rows[$i][2]) : '';
        $rowOut  = isset($rows[$i][4]) ? trim($rows[$i][4]) : '';

        if (
            $rowUid === $uid &&
            $rowDate === $date &&
            $rowOut === ''
        ) {

            // Update this open IN record
            $rows[$i][0] = $uid;
            $rows[$i][1] = $name;
            $rows[$i][2] = $date;
            $rows[$i][4] = $timeOut;
            $rows[$i][5] = $duration;

            $found = true;
            break;
        }
    }


    // ----------------------------------------------
    // If no open IN record was found
    // ----------------------------------------------

    if (!$found) {

        // Create a new row instead of overwriting anything
        $rows[] = [
            $uid,
            $name,
            $date,
            $timeIn,
            $timeOut,
            $duration
        ];

        $message = "New OUT record created";
    }
    else {

        $message = "OUT record updated";
    }
}


// ==================================================
// IN
// Always create a NEW row
// ==================================================

else {

    $rows[] = [
        $uid,
        $name,
        $date,
        $timeIn,
        '',
        ''
    ];

    $message = "New IN record created";
}


// --------------------------------------------------
// Rewrite CSV
// --------------------------------------------------

$fp = fopen($csvFile, 'w');

if ($fp === false) {
    http_response_code(500);
    echo "Cannot write CSV file";
    exit;
}

// UTF-8 BOM
fwrite($fp, "\xEF\xBB\xBF");

// Header
fputcsv($fp, [
    'UID',
    'Name',
    'DateJalali',
    'TimeIn',
    'TimeOut',
    'Duration(min)'
]);

// Data
foreach ($rows as $row) {

    // Make sure row has exactly 6 columns
    while (count($row) < 6) {
        $row[] = '';
    }

    fputcsv($fp, array_slice($row, 0, 6));
}

fclose($fp);


// --------------------------------------------------
// Response
// --------------------------------------------------

echo $message;

?>