(function(){
'use strict';
if (window.top !== window || globalThis.origin === 'null' || globalThis.__edenFormsInstalled) { return; }
globalThis.__edenFormsInstalled = true;
globalThis.__edenFormToken = 'EDEN_FORM_TOKEN';
if (!Object.hasOwn(globalThis, '__edenAutofillDocument')) {
    Object.defineProperty(globalThis, '__edenAutofillDocument', {value:Array.from(crypto.getRandomValues(new Uint32Array(4))).map(function(value){return value.toString(16).padStart(8,'0');}).join('')});
}
var send = function(kind, values) {
    window.webkit.messageHandlers.forms.postMessage({token:globalThis.__edenFormToken,documentId:globalThis.__edenAutofillDocument,origin:globalThis.origin,kind:kind,values:values});
};
var reportCredential = function(username, password){send('credential',[username,password]);};
var reportFormField = function(type,name,autocomplete,value,x,y,width,height){send('field',[type,name,autocomplete,value,x,y,width,height]);};
var visible = function(field){ return field && !field.disabled && !field.readOnly && field.getClientRects().length > 0; };
var fieldsFor = function(root){ return Array.prototype.slice.call((root || document).querySelectorAll('input,textarea')); };
var autocompleteHas = function(field, token){ return String(field.autocomplete || '').toLowerCase().split(/\s+/).indexOf(token) >= 0; };
var fieldIdentity = function(field){
    return [field.name, field.id, field.placeholder, field.getAttribute('aria-label')].filter(Boolean).join(' ').toLowerCase();
};
var usernameField = function(field){
    return field && field.value &&
        (autocompleteHas(field, 'username') || autocompleteHas(field, 'email') || field.type === 'email' ||
         /user|email|login|identifier/.test(fieldIdentity(field))) && visible(field);
};
var secretType = function(field){
    if (!field || autocompleteHas(field, 'one-time-code')) { return false; }
    if (field.type === 'password' || autocompleteHas(field, 'current-password') || autocompleteHas(field, 'new-password')) {
        return true;
    }
    return /password|passwd|passphrase|token|api[ _-]*key|secret|access[ _-]*key/.test(fieldIdentity(field));
};
var secretField = function(field){ return field && field.value && secretType(field) && visible(field); };
var usernameCandidate = function(root){
    var field = fieldsFor(root).find(usernameField);
    return field ? field.value || '' : '';
};
var usernameFor = function(secret){
    var form = secret.form || document;
    var explicit = usernameCandidate(form);
    if (explicit) { return explicit; }
    var fields = fieldsFor(form);
    var index = fields.indexOf(secret);
    for (var position = index - 1; position >= 0; --position) {
        var field = fields[position];
        if (visible(field) && /^(text|email|tel|$)/.test(field.type || '')) { return field.value || ''; }
    }
    return '';
};
var capture = function(root){
    var secret = fieldsFor(root).find(secretField);
    var username = secret ? usernameFor(secret) : usernameCandidate(root);
    if (username || secret) { try { reportCredential(username, secret ? secret.value : ''); } catch (error) {} }
};
document.addEventListener('submit', function(event){ if (event.isTrusted) { capture(event.target); } }, true);
document.addEventListener('click', function(event){
    if (!event.isTrusted) { return; }
    var target = event.target && event.target.closest ? event.target.closest('button,input,[role="button"],a') : null;
    if (!target) { return; }
    var label = [target.textContent, target.value, target.getAttribute('aria-label')].filter(Boolean).join(' ');
    var root = target.form || (target.closest ? target.closest('form') : null) || document;
    var buttonLike = /^(BUTTON|INPUT)$/.test(target.tagName) || target.getAttribute('role') === 'button';
    var activatesLogin = /sign.?in|log.?in|continue|next|submit|authenticate|unlock/i.test(label);
    var secondaryControl = /show|hide|reveal|visibility|cancel|back/i.test(label);
    if (target.type === 'submit' || activatesLogin || (buttonLike && !secondaryControl && fieldsFor(root).some(secretField))) {
        capture(root);
    }
}, true);
document.addEventListener('change', function(event){
    if (!event.isTrusted) { return; }
    var field = event.target;
    if (field && /^(INPUT|TEXTAREA)$/.test(field.tagName) && usernameField(field)) {
        try { reportCredential(field.value || '', ''); } catch (error) {}
    }
}, true);
document.addEventListener('keydown', function(event){
    if (event.isTrusted && event.key === 'Enter') { capture(event.target.form || document); }
}, true);
var relevantField = function(field){
    if (!field || !/^(INPUT|TEXTAREA)$/.test(field.tagName) || field.disabled || field.readOnly) { return false; }
    if (/^(password|email|tel)$/.test(field.type)) { return true; }
    var identity = fieldIdentity(field) + ' ' + String(field.autocomplete || '').toLowerCase();
    return /name|email|user|login|identifier|pass|token|secret|access|phone|address|city|state|province|postal|zip|country/
        .test(identity);
};
var requestFrame = window.requestAnimationFrame.bind(window);
var cancelFrame = window.cancelAnimationFrame.bind(window);
var pendingFrame = null;
var pendingField = null;
var lastReport = null;
var clearField = function(){
    if (pendingFrame !== null) { cancelFrame(pendingFrame); }
    pendingFrame = null;
    pendingField = null;
    if (lastReport) {
        lastReport = null;
        try { reportFormField('', '', '', '', 0, 0, 0, 0); } catch (error) {}
    }
};
var flushField = function(){
    pendingFrame = null;
    var field = pendingField;
    pendingField = null;
    if (!relevantField(field) || document.activeElement !== field) { clearField(); return; }
    var rect = field.getBoundingClientRect();
    if (rect.width <= 0 || rect.height <= 0) { clearField(); return; }
    var type = field.type || 'text';
    var value = type === 'password' || secretType(field) ? '' : field.value || '';
    var name = field.name || field.id || '';
    var autocomplete = field.autocomplete || '';
    var values = [type, name, autocomplete, value, rect.x, rect.y, rect.width, rect.height];
    if (lastReport) {
        var changed = false;
        for (var index = 0; index < values.length; ++index) {
            if (values[index] !== lastReport[index]) { changed = true; break; }
        }
        if (!changed) { return; }
    }
    lastReport = values;
    try {
        reportFormField(type, name, autocomplete, value, rect.x, rect.y, rect.width, rect.height);
    } catch (error) {}
};
var queueField = function(event){
    if (!event.isTrusted) { return; }
    if (!relevantField(event.target)) { clearField(); return; }
    pendingField = event.target;
    if (pendingFrame === null) { pendingFrame = requestFrame(flushField); }
};
document.addEventListener('focusin', function(event){
    queueField(event);
}, true);
document.addEventListener('contextmenu', function(event){
    if (!event.isTrusted || !relevantField(event.target)) { return; }
    event.target.focus({preventScroll:true});
    if (pendingFrame !== null) { cancelFrame(pendingFrame); }
    pendingField = event.target;
    flushField();
}, true);
document.addEventListener('focusout', function(event){ if (event.isTrusted) { clearField(); } }, true);
document.addEventListener('input', function(event){
    queueField(event);
}, true);
window.addEventListener('blur', clearField);
})();
