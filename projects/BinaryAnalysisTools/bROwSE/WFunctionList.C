#include <rose.h>
#include <rose_isnan.h>
#include <bROwSE/WAddressSpace.h>
#include <bROwSE/WFunctionList.h>

#include <bROwSE/FunctionListModel.h>
#include <Wt/WHBoxLayout>
#include <Wt/WText>
#include <Wt/WVBoxLayout>

using namespace Rose;
using namespace Rose::BinaryAnalysis;

namespace bROwSE {

void
WFunctionList::init() {
    Wt::WVBoxLayout *vbox = new Wt::WVBoxLayout;
    setLayout(vbox);

    // Address space
    vbox->addWidget(new Wt::WText("Representation of the address space. The top bar represents the memory map segments "
                                  "where green is executable (faded means also writable); yellow is non-executable and "
                                  "readable (faded means also writable); dark red means no permissions (faded means write-only); "
                                  "and black represents unmapped areas.  Clicking in the gutter area above the bar will zoom "
                                  "into a single segment.  The lower bar represents functions and is a heat-map based on "
                                  "whatever table column is currently sorted: dark blues for low values, and light red for "
                                  "high values; pure red means no value. Clicking in the gutter area below the functions "
                                  "will select the function.  Clicking within a bar graph zooms in, or out (with Control), "
                                  "or all the way out (with Shift-Control)."));

    wAddressSpace_ = new WAddressSpace;
    wAddressSpace_->insertSegmentsAndFunctions(ctx_.partitioner); // segments are bar0, functions are bar1
    wAddressSpace_->topGutterClicked().connect(this, &WFunctionList::selectSegmentByAddress);
    wAddressSpace_->bottomGutterClicked().connect(this, &WFunctionList::selectFunctionByAddress);
    vbox->addWidget(wAddressSpace_);

    // Container for the function table, with a horizontal layout so the table scrolls horizontally
    Wt::WContainerWidget *wTableContainer = new Wt::WContainerWidget;
    vbox->addWidget(wTableContainer, 1 /*stretch*/);
    Wt::WHBoxLayout *hbox = new Wt::WHBoxLayout;
    wTableContainer->setLayout(hbox);

    // Function table model. The order that analyzers are registered is the order that their columns will appear in the table.
    model_ = new FunctionListModel(ctx_);
    model_->analyzers().push_back(FunctionEntryAddress::instance()); // should normally be first since it's a bit special
    model_->analyzers().push_back(FunctionName::instance());
    model_->analyzers().push_back(FunctionSizeBytes::instance());
    model_->analyzers().push_back(FunctionSizeInsns::instance());
    model_->analyzers().push_back(FunctionSizeBBlocks::instance());
    model_->analyzers().push_back(FunctionSizeDBlocks::instance());
    model_->analyzers().push_back(FunctionNDiscontiguousBlocks::instance());
    model_->analyzers().push_back(FunctionNIntervals::instance());
    model_->analyzers().push_back(FunctionImported::instance());
    model_->analyzers().push_back(FunctionExported::instance());
    model_->analyzers().push_back(FunctionNCallers::instance());
    model_->analyzers().push_back(FunctionNCallees::instance());
    model_->analyzers().push_back(FunctionNReturns::instance());
    model_->analyzers().push_back(FunctionMayReturn::instance());
    model_->analyzers().push_back(FunctionStackDelta::instance());
    model_->analyzers().push_back(FunctionCallingConvention::instance());

    // Function table
    tableView_ = new Wt::WTableView;
    tableView_->setModel(model_);
    tableView_->setRowHeaderCount(1); // this must be first property set
    tableView_->setHeaderHeight(28);
    tableView_->setSortingEnabled(true);
    tableView_->setAlternatingRowColors(true);
    for (size_t i=0; i<model_->analyzers().size(); ++i)
        tableView_->setColumnWidth(i, model_->analyzers()[i]->naturalWidth());
    tableView_->setColumnResizeEnabled(true);
    tableView_->setSelectionMode(Wt::SingleSelection);
    tableView_->setEditTriggers(Wt::WAbstractItemView::NoEditTrigger);
    tableView_->      clicked().connect(this, &WFunctionList::selectFunctionByRow1);
    tableView_->doubleClicked().connect(this, &WFunctionList::selectFunctionByRow2);
    tableView_->headerClicked().connect(this, &WFunctionList::updateFunctionHeatMaps);
    hbox->addWidget(tableView_);

    // Startup. Initialize the ATTR_Heat values so they're not returned as NaN on the first redraw.
    BOOST_FOREACH (const P2::Function::Ptr &function, ctx_.partitioner.functions())
        function->setAttribute(ATTR_Heat, 0.0);
    updateFunctionHeatMaps();
}

void
WFunctionList::reload() {
    model_->reload();
    wAddressSpace_->clear();
    wAddressSpace_->insertSegmentsAndFunctions(ctx_.partitioner);
    wAddressSpace_->redraw();
    function_ = P2::Function::Ptr();                    // FIXME[Robb P. Matzke 2014-12-28]: emit functionChanged(NULL)?
}

const std::vector<P2::Function::Ptr>&
WFunctionList::functions() const {
    ASSERT_not_null(model_);
    return model_->functions();
}

// Internal: called when top gutter is clicked. Zoom to segment and emit segmentAddressClicked
void
WFunctionList::selectSegmentByAddress(rose_addr_t va, const Wt::WMouseEvent &event) {
    boost::iterator_range<MemoryMap::ConstNodeIterator> found = ctx_.partitioner.memoryMap()->at(va).nodes();
    if (found.begin() != found.end()) {
        const AddressInterval &interval = found.begin()->key();
        wAddressSpace_->displayedDomain(interval);
        wAddressSpace_->redraw();
        segmentAddressClicked_.emit(va, event);
    }
}

// Internal: called when bottom gutter is clicked. Choose a function and make it the current function. Emit
// functionAddressClicked.
void
WFunctionList::selectFunctionByAddress(rose_addr_t va, const Wt::WMouseEvent &event) {
    std::vector<P2::Function::Ptr> functions = ctx_.partitioner.functionsOverlapping(va);
    if (!functions.empty()) {
        changeFunction(functions.front());
        functionAddressClicked_.emit(functions.front(), va, event);
    }
}

// Internal: called when a table row is clicked.  Make function current and emit functionRowClicked.
void
WFunctionList::selectFunctionByRow1(const Wt::WModelIndex &idx) {
    if (idx.isValid()) {
        if (P2::Function::Ptr function = model_->functionAt(idx.row())) {
            changeFunction(function);
            functionRowClicked_.emit(function);
        }
    }
}

// Internal: same, but emit functionRowDoubleClicked
void
WFunctionList::selectFunctionByRow2(const Wt::WModelIndex &idx) {
    if (idx.isValid()) {
        if (P2::Function::Ptr function = model_->functionAt(idx.row())) {
            changeFunction(function);
            functionRowDoubleClicked_.emit(function);
        }
    }
}

// Make function the current function. Highlight its addresses in the address space, select it in the table, and emit a
// functionChanged signal.  Do nothing if it's already the current function.
void
WFunctionList::changeFunction(const P2::Function::Ptr &function) {
    if (function == function_)
        return;
    function_ = function;

    if (!function) {
        wAddressSpace_->highlights(FUNCTION_BAR).clear();
        wAddressSpace_->redraw();
        tableView_->setSelectedIndexes(Wt::WModelIndexSet());// deselect everything
        return;                                         // return without emitting functionChanged signal
    }

    // Update the address space
    wAddressSpace_->highlights(FUNCTION_BAR) = ctx_.partitioner.functionExtent(function);
    wAddressSpace_->redraw();

    // Update the table
    Wt::WModelIndex idx = model_->functionIdx(function);
    if (idx.isValid() && !tableView_->isSelected(idx)) {
        tableView_->select(idx);
        tableView_->scrollTo(idx);
    }
    
    functionChanged_.emit(function);
}

// Redraw the address space map. This is normally called when a table column is sorted.
void
WFunctionList::updateFunctionHeatMaps() {
    static const size_t functionIdx = 1;
    double minHeat=0, maxHeat=0;
    if (model_->heatStats().count()>0) {
        minHeat = model_->heatStats().minimum();
        maxHeat = model_->heatStats().maximum();
    }

    Color::Gradient g(Color::HSV(2/6.0, 0.3, 0.3), Color::HSV(1.0, 1.0, 0.75));
    wAddressSpace_->gradient(functionIdx).clear();
    if (minHeat == maxHeat) {
        wAddressSpace_->gradient(functionIdx).insert(0, g(0.5));
    } else {
        double p5  = model_->heatStats().p5();
        double p50 = model_->heatStats().p50();
        double p95 = model_->heatStats().p95();
        ASSERT_forbid(rose_isnan(p5) || rose_isnan(p50) || rose_isnan(p95));

        // Inserted in increasing order of importance in case some values overlap
        wAddressSpace_->gradient(functionIdx).insert(p5,      g(0.05));
        wAddressSpace_->gradient(functionIdx).insert(p95,     g(0.95));
        wAddressSpace_->gradient(functionIdx).insert(p50,     g(0.50));
        wAddressSpace_->gradient(functionIdx).insert(minHeat, g(0.00));
        wAddressSpace_->gradient(functionIdx).insert(maxHeat, g(1.00));
    }
    wAddressSpace_->bottomGutterGradient() = wAddressSpace_->gradient(functionIdx);
    wAddressSpace_->insert(ctx_.partitioner, ctx_.partitioner.functions());
    wAddressSpace_->redraw();
}

} // namespace
