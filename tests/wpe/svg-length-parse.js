window.edenSvgLengthParseRegression = async function () {
    const namespace = "http://www.w3.org/2000/svg";
    const results = [];
    const svg = document.createElementNS(namespace, "svg");
    svg.setAttribute("width", "200");
    svg.setAttribute("height", "200");

    function verify(name, condition, observed) {
        const record = {name, passed: Boolean(condition), observed};
        results.push(record);
        if (!record.passed) {
            throw new Error("SVG length regression failed: " + name + " observed=" + JSON.stringify(observed));
        }
    }

    const rect = document.createElementNS(namespace, "rect");
    rect.setAttribute("x", "12");
    rect.setAttribute("y", "4");
    rect.setAttribute("width", "80");
    rect.setAttribute("height", "60");
    svg.appendChild(rect);
    document.body.appendChild(svg);

    const observer = new MutationObserver(() => {});
    observer.observe(rect, {attributes: true, attributeFilter: ["x"]});
    rect.setAttribute("x", "12");
    const firstEqualWriteRecords = observer.takeRecords();
    rect.setAttribute("x", "12");
    const secondEqualWriteRecords = observer.takeRecords();
    verify(
        "repeated equal attribute writes preserve mutation notifications",
        firstEqualWriteRecords.length === 1 && secondEqualWriteRecords.length === 1,
        [firstEqualWriteRecords.length, secondEqualWriteRecords.length]
    );

    rect.setAttribute("x", "13");
    verify("changed attribute value reparses", rect.x.baseVal.valueInSpecifiedUnits === 13, rect.x.baseVal.valueInSpecifiedUnits);

    rect.removeAttribute("x");
    verify("attribute removal resets the reflected length", !rect.hasAttribute("x") && rect.x.baseVal.valueInSpecifiedUnits === 0, {
        hasAttribute: rect.hasAttribute("x"),
        value: rect.x.baseVal.valueInSpecifiedUnits
    });

    rect.setAttribute("x", "");
    verify("empty length uses its default value", rect.x.baseVal.valueInSpecifiedUnits === 0, rect.x.baseVal.valueInSpecifiedUnits);

    rect.setAttribute("x", "not-a-length");
    verify("invalid length remains observable", rect.getAttribute("x") === "not-a-length" && rect.x.baseVal.valueInSpecifiedUnits === 0, {
        attribute: rect.getAttribute("x"),
        value: rect.x.baseVal.valueInSpecifiedUnits
    });

    rect.setAttribute("width", "-4");
    verify(
        "negative forbidden length remains observable",
        rect.getAttribute("width") === "-4" && rect.width.baseVal.valueInSpecifiedUnits === -4,
        {attribute: rect.getAttribute("width"), value: rect.width.baseVal.valueInSpecifiedUnits}
    );

    rect.setAttribute("x", "14px");
    rect.setAttribute("x", "14px");
    verify(
        "pixel units survive repeated writes",
        rect.x.baseVal.unitType === 5 && rect.x.baseVal.valueInSpecifiedUnits === 14,
        {unitType: rect.x.baseVal.unitType, value: rect.x.baseVal.valueInSpecifiedUnits}
    );

    rect.setAttribute("height", "25%");
    rect.setAttribute("height", "25%");
    verify(
        "percentage units survive repeated writes",
        rect.height.baseVal.unitType === 2 && rect.height.baseVal.valueInSpecifiedUnits === 25,
        {unitType: rect.height.baseVal.unitType, value: rect.height.baseVal.valueInSpecifiedUnits}
    );

    rect.setAttribute("x", "calc(2px + 3px)");
    verify(
        "calc length is rejected",
        rect.getAttribute("x") === "calc(2px + 3px)" && rect.x.baseVal.valueInSpecifiedUnits === 0,
        {attribute: rect.getAttribute("x"), value: rect.x.baseVal.valueInSpecifiedUnits}
    );

    rect.setAttribute("x", "20%");
    rect.x.baseVal.newValueSpecifiedUnits(5, 30);
    const synchronizedValue = rect.getAttribute("x");
    rect.setAttribute("x", synchronizedValue);
    verify(
        "baseVal writes synchronize before an equal attribute write",
        synchronizedValue === "30px" && rect.x.baseVal.unitType === 5 && rect.x.baseVal.valueInSpecifiedUnits === 30,
        {attribute: synchronizedValue, unitType: rect.x.baseVal.unitType, value: rect.x.baseVal.valueInSpecifiedUnits}
    );

    const clone = rect.cloneNode(false);
    svg.appendChild(clone);
    clone.setAttribute("x", clone.getAttribute("x"));
    clone.x.baseVal.newValueSpecifiedUnits(1, 7);
    verify(
        "cloned element has independent animated length state",
        clone.x.baseVal.valueInSpecifiedUnits === 7 && rect.x.baseVal.valueInSpecifiedUnits === 30,
        {cloneValue: clone.x.baseVal.valueInSpecifiedUnits, sourceValue: rect.x.baseVal.valueInSpecifiedUnits}
    );

    const taggedLengths = [
        ["circle", "cx", "16"],
        ["line", "x1", "17"],
        ["ellipse", "rx", "18"],
        ["use", "width", "19"],
        ["image", "width", "20"],
        ["foreignObject", "width", "21"]
    ];
    for (const [tagName, attribute, value] of taggedLengths) {
        const element = document.createElementNS(namespace, tagName);
        element.setAttribute(attribute, value);
        svg.appendChild(element);
        element.setAttribute(attribute, value);
        const length = element[attribute].baseVal;
        verify(
            tagName + " repeated length attribute remains reflected",
            element.getAttribute(attribute) === value && length.valueInSpecifiedUnits === Number(value),
            {attribute: element.getAttribute(attribute), value: length.valueInSpecifiedUnits}
        );
    }

    const animatedRect = document.createElementNS(namespace, "rect");
    animatedRect.setAttribute("x", "10");
    const animate = document.createElementNS(namespace, "animate");
    animate.setAttribute("attributeName", "x");
    animate.setAttribute("from", "20");
    animate.setAttribute("to", "40");
    animate.setAttribute("dur", "4s");
    animate.setAttribute("fill", "freeze");
    animatedRect.appendChild(animate);
    svg.appendChild(animatedRect);
    animate.beginElement();
    svg.setCurrentTime(1);
    const animatedValue = animatedRect.x.animVal.value;
    animatedRect.setAttribute("x", "10");
    const baseValueAfterWrite = animatedRect.x.baseVal.value;
    const animatedValueAfterWrite = animatedRect.x.animVal.value;
    verify(
        "active SMIL animation retains parser update semantics",
        animatedValue > 10 && baseValueAfterWrite === 10 && animatedValueAfterWrite === baseValueAfterWrite,
        {animatedValue, baseValueAfterWrite, animatedValueAfterWrite}
    );
    svg.setCurrentTime(2);
    verify("active SMIL animation continues after the equal attribute write", animatedRect.x.animVal.value > 10, animatedRect.x.animVal.value);

    observer.disconnect();
    svg.remove();
    return results;
};
