process.parentPort.once('message', ({ data }) => {
  if (data === 'quit') process.exit(0);
});
process.parentPort.postMessage('utility started');
