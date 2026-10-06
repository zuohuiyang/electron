const { app, crashReporter } = require('electron');

console.log(
  JSON.stringify({
    ready: app.isReady(),
    results: [crashReporter.cleanup(), crashReporter.cleanup()]
  })
);
app.quit();
