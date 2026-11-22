// Rotellone condiviso: lo mostra durante ogni navigazione tra pagine.
(function () {
  var CSS = "#spinner{position:fixed;inset:0;background:rgba(8,10,14,.6);display:flex;align-items:center;justify-content:center;z-index:99;backdrop-filter:blur(2px)}#spinner.hidden{display:none}#spinner .ring{width:64px;height:64px;border:5px solid rgba(255,255,255,.15);border-top-color:#4fd1c5;border-radius:50%;animation:nzspin .8s linear infinite}@keyframes nzspin{to{transform:rotate(360deg)}}";
  if (!document.getElementById("spinner")) {
    var st = document.createElement("style"); st.textContent = CSS; document.head.appendChild(st);
    var d = document.createElement("div"); d.id = "spinner"; d.className = "hidden";
    var r = document.createElement("div"); r.className = "ring"; d.appendChild(r);
    document.body.appendChild(d);
  }
  window.nzShowSpinner = function () {
    var e = document.getElementById("spinner"); if (e) e.classList.remove("hidden");
  };
  document.addEventListener("DOMContentLoaded", function () {
    var links = document.querySelectorAll("a[href]");
    for (var i = 0; i < links.length; i++) {
      (function (a) {
        a.addEventListener("click", function () {
          if (!a.target) window.nzShowSpinner();
        });
      })(links[i]);
    }
  });
})();
