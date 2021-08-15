const char MAIN_page[] PROGMEM = R"=====(
<!DOCTYPE html>
<html>
<body>
<style>
input[type=checkbox] {
  display: none;
}

input[type=checkbox] + label {
  display: inline-block;
  background-color: #DB574D;
  color: white;
  font-family: sans-serif;
  font-size: 14px;
  font-weight: bold;
  height: 30px;
  line-height: 30px;
  position: relative;
  width: 80px;
}

input[type=checkbox] + label,
input[type=checkbox] + label i {
  -webkit-transition: all 200ms ease;
  -moz-transition: all 200ms ease;
  -o-transition: all 200ms ease;
  transition: all 200ms ease;
}

input[type=checkbox]:checked + label {
  background-color: #67B04F;
}

input[type=checkbox] + label:before,
input[type=checkbox] + label:after,
input[type=checkbox] + label i {
  width: 50%;
  display: inline-block;
  height: 100%;
  text-align: center;
}

input[type=checkbox] + label:before {
  content: attr(data-text-true);
}

input[type=checkbox] + label:after {
  content: attr(data-text-false);
}

input[type=checkbox] + label i {
  top: 10%;
  background-color: white;
  height: 80%;
  left: 5%;
  position: absolute;
  width: 45%;
}

input[type=checkbox]:checked + label i {
  left: 50%;
}
</style>

<script>
function sendData(field, onSuccess = ()=>{}, value = null) {
  if (!value) {
    value = document.getElementById(field).value;
  }
  var xhttp = new XMLHttpRequest();
  xhttp.onreadystatechange = function() {
    if (this.readyState == 4 && this.status == 200) {
      onSuccess();
    }
  };
  xhttp.open("GET", "setValue?value="+value+"&field="+field, true);
  xhttp.send();
}

function sendMistDuration() {
  var value = document.getElementById('MistDuration').value;
  // Convert seconds to millis
  value *= 1000;
  sendData('MistDuration', getMistDuration, value);
}

function sendMistInterval() {
  var value = document.getElementById('MistInterval').value;
  // Convert minutes to millis
  value *= 1000 * 60;
  sendData('MistInterval', getMistInterval, value);
}

function sendMinPSI() {
  sendData('MinPSI', getMinPSI);
}

function sendMaxPSI() {
  sendData('MaxPSI', getMaxPSI);
}

function sendMistStatus() {
  let value = document.getElementById("MistStatus").checked ? "1" : "0";
  sendData('MistStatus', getMistStatus, value);
}

function sendPumpStatus() {
  let value = document.getElementById("PumpStatus").checked ? "1" : "0";
  sendData('PumpStatus', getPumpStatus, value);
}

function getSwitchValue(field) {
  var xhttp = new XMLHttpRequest();
  xhttp.onreadystatechange = function() {
    if (this.readyState == 4 && this.status == 200) {
      document.getElementById(field).checked = this.responseText == "1";
    }
  };
  xhttp.open("GET", "getValue?field="+field, true);
  xhttp.send();
}

function getMistStatus() {
  getSwitchValue('MistStatus');
}

function getPumpStatus() {
  getSwitchValue('PumpStatus');
}

// Retrieves the specified field and sets that field's value.
// Optionally applies a transformation to the received value.
async function getValue(field, transformation = (x) => x) {
  var xhttp = new XMLHttpRequest();
  xhttp.onreadystatechange = function() {
    if (this.readyState == 4 && this.status == 200) {
      document.getElementById(field).value = transformation(this.responseText);
    }
  };
  xhttp.open("GET", "getValue?field="+field, true);
  xhttp.send();
}

function getMistDuration() {
  // Convert millis to seconds
  getValue('MistDuration', (x) => x / 1000.0);
}

function getMistInterval() {
  // Convert millis to minutes
  getValue('MistInterval', (x) => x / (60 * 1000.0));
}

function getMinPSI() {
  getValue('MinPSI');
}

function getMaxPSI() {
  getValue('MaxPSI');
}

async function getPressure() {
  var xhttp = new XMLHttpRequest();
  xhttp.onreadystatechange = function() {
    if (this.readyState == 4 && this.status == 200) {
      document.getElementById("Pressure").innerHTML = this.responseText;
    }
  };
  xhttp.open("GET", "getValue?field=Pressure", true);
  xhttp.send();
}

setInterval(function() {
  getPressure().catch(console.log);
}, 5000); // update every 5 seconds

</script>

<div id="Misting">
<h1>Misting</h1>
<div>
    <label for="MistDuration">Misting:</label>
    <input type="checkbox" id="MistStatus" onclick="sendMistStatus()">
    <label for="MistStatus" data-text-true="ON" data-text-false="OFF"><i></i></label>
    <script type="text/javascript"> getMistStatus(); </script>
</div>
<form action="javascript:sendMistDuration()">
  <div>
    <label for="MistDuration">Misting Duration (seconds):</label>
    <input id="MistDuration" type="number" name="value" step="0.5" min="0.5" max="30" required>
    <script type="text/javascript"> getMistDuration(); </script>
    <span class="validity"></span>
    <input type="submit">
  </div>
</form>
<form action="javascript:sendMistInterval()">
  <div>
    <label for="MistInterval">Misting Interval (minutes):</label>
    <input id="MistInterval" type="number" name="value" step="1" min="1" max="30" required>
    <script type="text/javascript"> getMistInterval(); </script>
    <span class="validity"></span>
    <input type="submit">
  </div>
</form>

</div>

<h1>Pump</h1>
<div>
  <label for="PumpStatus">Pump:</label>
  <input type="checkbox" id="PumpStatus" onclick="sendPumpStatus()">
  <label for="PumpStatus" data-text-true="ON" data-text-false="OFF"><i></i></label>
  <script type="text/javascript"> getPumpStatus(); </script>
</div>
<div>
  Current pressure: <span id="Pressure"></span>
</div>
<form action="javascript:sendMinPSI()">
  <div>
    <label for="MinPSI">Min PSI:</label>
    <input id="MinPSI" type="number" name="value" step="1" min="50" max="110" required>
    <script type="text/javascript"> getMinPSI(); </script>
    <span class="validity"></span>
    <input type="submit">
  </div>
</form>

<form action="javascript:sendMaxPSI()">
  <div>
    <label for="MaxPSI">Max PSI:</label>
    <input id="MaxPSI" type="number" name="value" step="1" min="60" max="115" required>
    <script type="text/javascript"> getMaxPSI(); </script>
    <span class="validity"></span>
    <input type="submit">
  </div>
</form>

</div>

</body>
</html>
)=====";
